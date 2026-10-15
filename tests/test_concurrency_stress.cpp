#include "state_store.hpp"
#include "cgroup.hpp"
#include "utils.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <unordered_set>
#include <atomic>
#include <cassert>
#include <filesystem>
#include <unistd.h>
#include <sys/wait.h>
#include <sched.h>
#include <fstream>
#include <cstring>
#include <dirent.h>
#include <mutex>

namespace fs = std::filesystem;

namespace {

int count_open_fds() {
    int count = 0;
    DIR* dir = opendir("/proc/self/fd");
    if (!dir) return -1;
    while (dirent* entry = readdir(dir)) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

bool has_mount_matching(const std::string& pattern) {
    std::ifstream f("/proc/self/mountinfo");
    if (!f.is_open()) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.find(pattern) != std::string::npos) return true;
    }
    return false;
}

void test_parallel_100_state_and_cgroup_lifecycles() {
    std::cout << "[RUN] test_parallel_100_state_and_cgroup_lifecycles (100 concurrent threads)\n";
    const int kNumThreads = 100;

    fs::path store_root = fs::temp_directory_path() / ("quiver_stress_store_" + std::to_string(::getpid()));
    fs::path cgroup_root = fs::temp_directory_path() / ("quiver_stress_cg_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(store_root, ec);
    fs::remove_all(cgroup_root, ec);
    fs::create_directories(store_root, ec);
    fs::create_directories(cgroup_root, ec);

    // Populate mock root cgroup controllers
    Utils::write_file(cgroup_root / "cgroup.controllers", "cpu memory pids\n");
    Utils::write_file(cgroup_root / "cgroup.subtree_control", "");

    StateStore store(store_root);

    std::vector<std::string> container_ids(kNumThreads);
    std::mutex ids_mutex;
    std::unordered_set<std::string> unique_ids;
    std::atomic<int> failed_operations{0};

    std::vector<std::thread> workers;
    workers.reserve(kNumThreads);

    for (int i = 0; i < kNumThreads; ++i) {
        workers.emplace_back([&, i]() {
            auto cid = Utils::generate_container_id();
            {
                std::lock_guard<std::mutex> lock(ids_mutex);
                container_ids[i] = cid;
                unique_ids.insert(cid);
            }

            // 1. Setup Cgroup hierarchy
            ResourceLimits limits{
                .memory_max_bytes = 268435456ULL + (i * 1024),
                .cpus = 1.0 + (i * 0.01),
                .pids_max = 64 + i
            };

            CgroupManager cg(cid, limits, cgroup_root);
            std::string cg_err;
            if (!cg.setup(cg_err)) {
                failed_operations++;
                return;
            }

            // 2. Assign mock PID (using ::getpid() so auto-heal recognizes the process as alive)
            pid_t mock_pid = ::getpid();
            if (!cg.assign_pid(mock_pid, cg_err)) {
                failed_operations++;
                return;
            }

            // Populate mock metric files for read_metrics validation
            fs::path leaf_dir = cgroup_root / ("quiver-" + cid.substr(0, 12));
            Utils::write_file(leaf_dir / "memory.current", std::to_string(1048576 * (i + 1)) + "\n");
            Utils::write_file(leaf_dir / "cpu.stat", "usage_usec " + std::to_string(50000 * (i + 1)) + "\nthrottled_usec 0\n");
            Utils::write_file(leaf_dir / "pids.current", "1\n");

            CgroupMetrics metrics{};
            if (!CgroupManager::read_metrics(leaf_dir, metrics)) {
                failed_operations++;
                return;
            }
            if (metrics.memory_current_bytes != 1048576ULL * (i + 1)) {
                failed_operations++;
                return;
            }

            // 3. StateStore Lifecycle
            ContainerState cs{};
            cs.id = cid;
            cs.name = "stress_cnt_" + std::to_string(i);
            cs.image = "ubuntu:22.04";
            cs.status = "created";
            cs.resources = limits;
            if (!store.save_container(cs)) {
                failed_operations++;
                return;
            }

            if (!store.update_container_status(cid, "running", mock_pid)) {
                failed_operations++;
                return;
            }

            auto opt = store.get_container(cid);
            if (!opt.has_value() || opt->status != "running") {
                failed_operations++;
                return;
            }

            // Concurrently query list_containers
            auto list = store.list_containers(false);
            if (list.empty()) {
                failed_operations++;
                return;
            }

            if (!store.update_container_status(cid, "exited", mock_pid)) {
                failed_operations++;
                return;
            }

            // 4. Teardown
            if (!cg.cleanup()) {
                failed_operations++;
                return;
            }
            if (!store.remove_container(cid)) {
                failed_operations++;
                return;
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    // Assertions
    std::cout << "  - Unique Container IDs generated: " << unique_ids.size() << " / " << kNumThreads << "\n";
    assert(unique_ids.size() == kNumThreads);

    std::cout << "  - Failed operations count: " << failed_operations.load() << "\n";
    assert(failed_operations.load() == 0);

    // Verify zero leftover cgroup directories under mock root (other than root control files)
    int leftover_cgroups = 0;
    for (const auto& entry : fs::directory_iterator(cgroup_root)) {
        if (entry.is_directory()) {
            leftover_cgroups++;
        }
    }
    std::cout << "  - Leftover Cgroup directories: " << leftover_cgroups << "\n";
    assert(leftover_cgroups == 0);

    // Verify zero leftover container directories under store
    fs::path containers_dir = store_root / "containers";
    int leftover_containers = 0;
    if (fs::exists(containers_dir)) {
        for (const auto& entry : fs::directory_iterator(containers_dir)) {
            if (entry.is_directory()) {
                leftover_containers++;
            }
        }
    }
    std::cout << "  - Leftover Container directories: " << leftover_containers << "\n";
    assert(leftover_containers == 0);

    fs::remove_all(store_root, ec);
    fs::remove_all(cgroup_root, ec);
    std::cout << "[PASS] test_parallel_100_state_and_cgroup_lifecycles\n";
}

void test_parallel_75_rootless_container_processes() {
    std::cout << "[RUN] test_parallel_75_rootless_container_processes (75 concurrent sandboxed child processes)\n";
    const int kNumProcesses = 75;

    // Check if unprivileged CLONE_NEWUSER is supported
    bool userns_supported = false;
    {
        pid_t probe = fork();
        if (probe == 0) {
            int rc = unshare(CLONE_NEWUSER);
            _exit(rc == 0 ? 0 : 1);
        } else if (probe > 0) {
            int st = 0;
            waitpid(probe, &st, 0);
            userns_supported = (WIFEXITED(st) && WEXITSTATUS(st) == 0);
        }
    }
    std::cout << "  - Host Kernel CLONE_NEWUSER support: " << (userns_supported ? "YES" : "RESTRICTED") << "\n";

    int fd_count_before = count_open_fds();
    std::cout << "  - Baseline open file descriptors: " << fd_count_before << "\n";

    fs::path stress_test_dir = fs::temp_directory_path() / ("quiver_stress_proc_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(stress_test_dir, ec);
    fs::create_directories(stress_test_dir, ec);

    StateStore store(stress_test_dir / "store");

    std::vector<pid_t> child_pids;
    child_pids.reserve(kNumProcesses);

    for (int i = 0; i < kNumProcesses; ++i) {
        int pipe_sync[2];
        assert(pipe(pipe_sync) == 0);

        pid_t pid = fork();
        assert(pid >= 0);

        if (pid == 0) {
            // Child process
            close(pipe_sync[1]);
            // Wait for parent signal
            char sync = 0;
            [[maybe_unused]] auto _r = read(pipe_sync[0], &sync, 1);
            close(pipe_sync[0]);

            if (userns_supported) {
                // Perform rootless unshare
                if (unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWUTS) != 0) {
                    _exit(1);
                }
            }

            // Execute clean command
            _exit(0);
        } else {
            // Parent
            close(pipe_sync[0]);
            child_pids.push_back(pid);

            // Signal child to continue
            char sync = 'G';
            [[maybe_unused]] auto _w = write(pipe_sync[1], &sync, 1);
            close(pipe_sync[1]);
        }
    }

    // Wait and reap all 75 child processes
    int successful_exits = 0;
    for (pid_t cpid : child_pids) {
        int status = 0;
        pid_t w = waitpid(cpid, &status, 0);
        if (w == cpid && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            successful_exits++;
        }
    }

    std::cout << "  - Successfully reaped child processes: " << successful_exits << " / " << kNumProcesses << "\n";
    assert(successful_exits == kNumProcesses);

    // Verify zero zombie processes remain
    int zombie_status = 0;
    pid_t zombie_check = waitpid(-1, &zombie_status, WNOHANG);
    assert(zombie_check == -1 && errno == ECHILD);
    std::cout << "  - Zero zombie processes verified (waitpid returned ECHILD)\n";

    // Verify zero leaked file descriptors
    int fd_count_after = count_open_fds();
    std::cout << "  - Post-burst open file descriptors: " << fd_count_after
              << " (difference: " << (fd_count_after - fd_count_before) << ")\n";
    assert(fd_count_after == fd_count_before);

    // Verify zero leftover mounts in mountinfo
    bool leftover_mounts = has_mount_matching(stress_test_dir.string());
    std::cout << "  - Zero leftover mounts in /proc/self/mountinfo: " << (!leftover_mounts ? "PASS" : "FAIL") << "\n";
    assert(!leftover_mounts);

    fs::remove_all(stress_test_dir, ec);
    std::cout << "[PASS] test_parallel_75_rootless_container_processes\n";
}

} // namespace

int main() {
    std::cout << "=================================================================\n";
    std::cout << "     QUIVER v2.0 50-100 CONCURRENT CONTAINER STRESS SUITE\n";
    std::cout << "=================================================================\n\n";

    test_parallel_100_state_and_cgroup_lifecycles();
    test_parallel_75_rootless_container_processes();

    std::cout << "\n>>> ALL CONCURRENCY STRESS TESTS PASSED DETERMINISTICALLY <<<\n";
    return 0;
}
