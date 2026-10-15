#include <cassert>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <filesystem>
#include <unistd.h>
#include <sys/wait.h>
#include "cgroup.hpp"
#include "utils.hpp"
#include "state_store.hpp"

namespace fs = std::filesystem;

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void test_memory_parser() {
    std::cout << "[RUN] test_memory_parser\n";
    assert(Utils::parse_memory_string("512m") == 536870912ULL);
    assert(Utils::parse_memory_string("2G") == 2147483648ULL);
    assert(Utils::parse_memory_string("1024k") == 1048576ULL);
    assert(Utils::parse_memory_string("4096") == 4096ULL);

    const std::vector<std::string> malformed = {"", "-512m", "0", "128x", "abc"};
    for (const auto& bad : malformed) {
        bool threw = false;
        try {
            (void)Utils::parse_memory_string(bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }
    std::cout << "[PASS] test_memory_parser\n";
}

static void test_cpu_and_pids_parsers() {
    std::cout << "[RUN] test_cpu_and_pids_parsers\n";
    assert(Utils::parse_cpus_string("0.5") == 0.5);
    assert(Utils::parse_cpus_string("2.5") == 2.5);

    const std::vector<std::string> invalid_cpus = {"0", "-1.0", "abc"};
    for (const auto& bad : invalid_cpus) {
        bool threw = false;
        try {
            (void)Utils::parse_cpus_string(bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }

    assert(Utils::parse_pids_limit("256") == 256);
    assert(Utils::parse_pids_limit("max") <= 0);

    const std::vector<std::string> invalid_pids = {"0", "-5", "xyz"};
    for (const auto& bad : invalid_pids) {
        bool threw = false;
        try {
            (void)Utils::parse_pids_limit(bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }
    std::cout << "[PASS] test_cpu_and_pids_parsers\n";
}

static void test_cgroup_formatting() {
    std::cout << "[RUN] test_cgroup_formatting\n";
    assert(CgroupManager::format_memory_max(536870912) == "536870912\n");
    assert(CgroupManager::format_memory_max(0) == "max\n");

    assert(CgroupManager::format_cpu_max(2.0, 100000) == "200000 100000\n");
    assert(CgroupManager::format_cpu_max(0.5, 100000) == "50000 100000\n");
    assert(CgroupManager::format_cpu_max(0.0, 100000) == "max 100000\n");

    assert(CgroupManager::format_pids_max(512) == "512\n");
    assert(CgroupManager::format_pids_max(0) == "max\n");
    std::cout << "[PASS] test_cgroup_formatting\n";
}

static void test_mock_cgroup_hierarchy_lifecycle() {
    std::cout << "[RUN] test_mock_cgroup_hierarchy_lifecycle\n";
    fs::path mock_root = fs::temp_directory_path() / ("quiver_cg_test_" + std::to_string(::getpid()));
    fs::remove_all(mock_root);
    fs::create_directories(mock_root);

    // Pre-populate cgroup.controllers and cgroup.subtree_control
    Utils::write_file(mock_root / "cgroup.controllers", "cpu memory pids\n");
    Utils::write_file(mock_root / "cgroup.subtree_control", "");

    ResourceLimits limits{
        .memory_max_bytes = 268435456,
        .cpus = 1.5,
        .pids_max = 64
    };
    CgroupManager cg("abcdef1234567890", limits, mock_root);
    std::string err;
    assert(cg.setup(err));

    fs::path leaf_dir = mock_root / "quiver-abcdef123456";
    assert(fs::exists(leaf_dir));

    assert(read_file(leaf_dir / "memory.max") == "268435456\n");
    assert(read_file(leaf_dir / "memory.swap.max") == "0\n");
    assert(read_file(leaf_dir / "cpu.max") == "150000 100000\n");
    assert(read_file(leaf_dir / "pids.max") == "64\n");

    assert(cg.assign_pid(4242, err));
    assert(read_file(leaf_dir / "cgroup.procs") == "4242\n");

    assert(cg.cleanup());
    assert(!fs::exists(leaf_dir));

    fs::remove_all(mock_root);
    std::cout << "[PASS] test_mock_cgroup_hierarchy_lifecycle\n";
}

static void test_state_store_resource_persistence() {
    std::cout << "[RUN] test_state_store_resource_persistence\n";
    fs::path store_dir = fs::temp_directory_path() / ("quiver_store_test_" + std::to_string(::getpid()));
    fs::remove_all(store_dir);

    StateStore store(store_dir);
    store.ensure_store_layout();

    ContainerState cs{};
    cs.id = "test-container-res-123";
    cs.name = "test-res";
    cs.image = "alpine:latest";
    cs.working_dir = "/workspace";
    cs.env = {"CUDA_VISIBLE_DEVICES=0", "OMP_NUM_THREADS=4"};
    cs.resources.memory_max_bytes = 1073741824ULL;
    cs.resources.cpus = 2.0;
    cs.resources.pids_max = 128;

    assert(store.save_container(cs));

    auto loaded = store.get_container("test-container-res-123");
    assert(loaded.has_value());
    assert(loaded->working_dir == "/workspace");
    assert((loaded->env == std::vector<std::string>{"CUDA_VISIBLE_DEVICES=0", "OMP_NUM_THREADS=4"}));
    assert(loaded->resources.memory_max_bytes == 1073741824ULL);
    assert(loaded->resources.cpus == 2.0);
    assert(loaded->resources.pids_max == 128);

    store.remove_container("test-container-res-123");
    fs::remove_all(store_dir);
    std::cout << "[PASS] test_state_store_resource_persistence\n";
}

static void test_live_kernel_cgroup_v2_delegation() {
    std::cout << "[RUN] test_live_kernel_cgroup_v2_delegation\n";
    if (!CgroupManager::is_cgroup_v2_available("/sys/fs/cgroup")) {
        std::cout << "[SKIP] test_live_kernel_cgroup_v2_delegation (cgroup v2 unavailable)\n";
        return;
    }

    std::string test_id = "testlive0011223344";
    ResourceLimits limits{
        .memory_max_bytes = 536870912ULL,
        .cpus = 1.0,
        .pids_max = 100
    };
    CgroupManager cg(test_id, limits);
    std::string err;
    if (!cg.setup(err)) {
        std::cout << "[SKIP] test_live_kernel_cgroup_v2_delegation (delegation unavailable: " << err << ")\n";
        return;
    }

    fs::path cg_dir = cg.cgroup_path();
    assert(fs::exists(cg_dir));

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ::usleep(200000); // 200ms
        _exit(0);
    }

    std::string assign_err;
    bool assigned = cg.assign_pid(pid, assign_err);
    assert(assigned);

    // Verify procs contains child PID
    std::ifstream procs_f(cg_dir / "cgroup.procs");
    bool found_pid = false;
    pid_t p = 0;
    while (procs_f >> p) {
        if (p == pid) {
            found_pid = true;
            break;
        }
    }
    assert(found_pid);

    int status = 0;
    waitpid(pid, &status, 0);

    assert(cg.cleanup());
    assert(!fs::exists(cg_dir));

    std::cout << "[PASS] test_live_kernel_cgroup_v2_delegation\n";
}

int main() {
    std::cout << "Starting Cgroup v2 Test Suite...\n";
    test_memory_parser();
    test_cpu_and_pids_parsers();
    test_cgroup_formatting();
    test_mock_cgroup_hierarchy_lifecycle();
    test_state_store_resource_persistence();
    test_live_kernel_cgroup_v2_delegation();
    std::cout << "All Cgroup v2 tests passed successfully!\n";
    return 0;
}
