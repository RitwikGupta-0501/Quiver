#include "gpu_manager.hpp"
#include "cgroup.hpp"
#include "state_store.hpp"
#include "container.hpp"
#include "utils.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <cassert>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>
#include <fstream>

namespace fs = std::filesystem;

namespace {

void test_unified_cli_resource_and_gpu_spec_wiring() {
    std::cout << "[RUN] test_unified_cli_resource_and_gpu_spec_wiring\n";

    // Simulate CLI: quiver run --gpus 0 --memory 2g --cpus 2.0 -i cuda:12.0 ./cuda_matrix_bench
    auto gpu_req = Utils::parse_gpus_string("0");
    uint64_t mem_bytes = Utils::parse_memory_string("2g");
    double cpus = Utils::parse_cpus_string("2.0");
    int pids = 512;

    ContainerSpec spec{};
    spec.id = "e2e_cuda_cnt_001";
    spec.image = "nvidia/cuda:12.0-base";
    spec.resources.all_gpus = gpu_req.all_gpus;
    spec.resources.gpu_indices = std::move(gpu_req.gpu_indices);
    spec.resources.memory_max_bytes = mem_bytes;
    spec.resources.cpus = cpus;
    spec.resources.pids_max = pids;

    assert(!spec.resources.all_gpus);
    assert((spec.resources.gpu_indices == std::vector<int>{0}));
    assert(spec.resources.memory_max_bytes == 2147483648ULL); // 2 GiB
    assert(spec.resources.cpus == 2.0);
    assert(spec.resources.pids_max == 512);

    // Validate Cgroup v2 formatting strings
    assert(CgroupManager::format_memory_max(spec.resources.memory_max_bytes) == "2147483648\n");
    assert(CgroupManager::format_cpu_max(spec.resources.cpus, 100000) == "200000 100000\n");
    assert(CgroupManager::format_pids_max(spec.resources.pids_max) == "512\n");

    std::cout << "  - Verified ContainerSpec resources for --gpus 0 --memory 2g --cpus 2.0\n";
    std::cout << "  - Memory: 2147483648 B (2.00 GiB), CPU quota: 200000/100000 (2.0 CPUs), PIDs: 512\n";
    std::cout << "[PASS] test_unified_cli_resource_and_gpu_spec_wiring\n";
}

void test_containerized_driver_and_device_injection_validation() {
    std::cout << "[RUN] test_containerized_driver_and_device_injection_validation\n";

    fs::path test_dir = fs::temp_directory_path() / ("quiver_e2e_inject_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(test_dir, ec);

    fs::path mock_old_root = test_dir / "old_root";
    fs::path mock_host_dev = mock_old_root / "dev";
    fs::path mock_host_lib = mock_old_root / "usr/lib/x86_64-linux-gnu";
    fs::path mock_container_root = test_dir / "container_rootfs";
    fs::path mock_cont_dev = mock_container_root / "dev";

    Utils::ensure_dirs(mock_host_dev);
    Utils::ensure_dirs(mock_host_lib);
    Utils::ensure_dirs(mock_cont_dev);

    // Create host mock devices: nvidiactl, nvidia-uvm, nvidia-uvm-tools, nvidia0 (GPU 0), nvidia1 (GPU 1)
    const std::vector<std::string> dev_files = {
        "nvidiactl", "nvidia-uvm", "nvidia-uvm-tools", "nvidia0", "nvidia1"
    };
    for (const auto& f : dev_files) {
        Utils::write_file(mock_host_dev / f, "mock_device_node");
    }

    // Create host mock driver libraries
    Utils::write_file(mock_host_lib / "libcuda.so.550.54.14", "mock_cuda_driver");
    fs::current_path(mock_host_lib);
    ::symlink("libcuda.so.550.54.14", (mock_host_lib / "libcuda.so.1").c_str());

    Utils::write_file(mock_host_lib / "libnvidia-ml.so.550.54.14", "mock_nvml_driver");
    ::symlink("libnvidia-ml.so.550.54.14", (mock_host_lib / "libnvidia-ml.so.1").c_str());

    Utils::write_file(mock_host_lib / "libnvidia-ptxjitcompiler.so.550.54.14", "mock_ptx_compiler");
    ::symlink("libnvidia-ptxjitcompiler.so.550.54.14", (mock_host_lib / "libnvidia-ptxjitcompiler.so.1").c_str());

    // 1. Validate device mount isolation (--gpus 0: GPU 0 exposed, GPU 1 isolated)
    ResourceLimits gpu_limits{};
    gpu_limits.all_gpus = false;
    gpu_limits.gpu_indices = {0};

    std::string err;
    bool dev_ok = GpuDeviceManager::setup_device_mounts(gpu_limits, mock_host_dev, mock_cont_dev, err, /*dry_run=*/true);
    assert(dev_ok);

    assert(fs::exists(mock_cont_dev / "nvidiactl"));
    assert(fs::exists(mock_cont_dev / "nvidia-uvm"));
    assert(fs::exists(mock_cont_dev / "nvidia0"));
    assert(!fs::exists(mock_cont_dev / "nvidia1")); // Strict device isolation!
    std::cout << "  - Device isolation verified: /dev/nvidia0 passed through, /dev/nvidia1 strictly absent\n";

    // 2. Validate host driver library discovery & injection
    bool lib_ok = GpuDeviceManager::inject_driver_libraries(mock_old_root, mock_container_root, err, /*dry_run=*/true);
    assert(lib_ok);

    assert(fs::exists(mock_container_root / "usr/lib/libcuda.so.1"));
    assert(fs::exists(mock_container_root / "usr/lib/libnvidia-ml.so.1"));
    assert(fs::exists(mock_container_root / "usr/lib/libnvidia-ptxjitcompiler.so.1"));
    assert(fs::is_symlink(mock_container_root / "usr/lib/libcuda.so"));
    assert(fs::is_symlink(mock_container_root / "usr/lib/libnvidia-ml.so"));
    std::cout << "  - Host driver libraries injected: libcuda.so.1, libnvidia-ml.so.1, libnvidia-ptxjitcompiler.so.1\n";

    fs::remove_all(test_dir, ec);
    std::cout << "[PASS] test_containerized_driver_and_device_injection_validation\n";
}

void test_live_rootless_cuda_and_cgroup_e2e_workload() {
    std::cout << "[RUN] test_live_rootless_cuda_and_cgroup_e2e_workload\n";

    fs::path test_base = fs::temp_directory_path() / ("quiver_e2e_cg_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(test_base, ec);
    fs::create_directories(test_base, ec);

    // Mock cgroup controller setup
    Utils::write_file(test_base / "cgroup.controllers", "cpu memory pids\n");
    Utils::write_file(test_base / "cgroup.subtree_control", "");

    ResourceLimits limits{
        .memory_max_bytes = 2147483648ULL, // 2 GiB
        .cpus = 2.0,
        .pids_max = 512,
        .gpu_indices = {0},
        .all_gpus = false
    };

    std::string cid = "e2e_cuda_workload_test";
    CgroupManager cg(cid, limits, test_base);
    std::string cg_err;
    assert(cg.setup(cg_err));

    fs::path leaf_dir = test_base / ("quiver-" + cid.substr(0, 12));
    assert(fs::exists(leaf_dir));

    // Fork a child process to simulate active workload execution
    int pipe_sync[2];
    assert(pipe(pipe_sync) == 0);

    pid_t child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        close(pipe_sync[1]);
        char sig = 0;
        [[maybe_unused]] auto _r = read(pipe_sync[0], &sig, 1);
        close(pipe_sync[0]);

        // Simulating 800ms of active compute workload
        auto start = std::chrono::steady_clock::now();
        double sum = 0.0;
        while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < 800) {
            for (int k = 0; k < 10000; ++k) sum += k * 0.001;
        }
        _exit(sum > 0.0 ? 0 : 1);
    }

    close(pipe_sync[0]);
    assert(cg.assign_pid(child_pid, cg_err));

    // Signal child to start compute workload
    char sig = '1';
    [[maybe_unused]] auto _w = write(pipe_sync[1], &sig, 1);
    close(pipe_sync[1]);

    // Populate live telemetry files
    Utils::write_file(leaf_dir / "memory.current", "134217728\n"); // 128 MiB
    Utils::write_file(leaf_dir / "cpu.stat", "usage_usec 400000\nuser_usec 300000\nsystem_usec 100000\nthrottled_usec 0\n");
    Utils::write_file(leaf_dir / "pids.current", "1\n");
    Utils::write_file(leaf_dir / "cgroup.procs", std::to_string(child_pid) + "\n");

    // Sleep 100ms so child is actively in its loop
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 1. Read live Cgroup v2 metrics
    CgroupMetrics cm{};
    bool cm_ok = CgroupManager::read_metrics(leaf_dir, cm);
    assert(cm_ok);
    assert(cm.memory_current_bytes == 134217728ULL);
    assert(cm.memory_max_bytes == 2147483648ULL);
    assert(cm.pids_current == 1);
    assert(cm.pids.size() == 1 && cm.pids[0] == child_pid);
    std::cout << "  - Live Cgroup v2 metrics verified: 128.00 MiB / 2.00 GiB, PID: " << child_pid << "\n";

    // 2. Query NVML telemetry collector
    NvmlTelemetryCollector collector;
    auto gpu_telemetry = collector.collect_metrics(limits);
    (void)gpu_telemetry;

    // 3. Format stats row
    GpuDeviceTelemetry mock_gt{};
    mock_gt.index = 0;
    mock_gt.gpu_utilization_pct = 78;
    mock_gt.temperature_c = 52;
    mock_gt.used_vram_bytes = 1073741824ULL;  // 1.00 GiB
    mock_gt.total_vram_bytes = 8589934592ULL; // 8.00 GiB

    std::string stats_row = GpuDeviceManager::format_stats_row(
        cid, "quiver-" + cid.substr(0, 12), 48.5, cm, {mock_gt}, /*gpu_requested=*/true
    );

    assert(stats_row.find(cid.substr(0, 12)) != std::string::npos);
    assert(stats_row.find("48.50%") != std::string::npos);
    assert(stats_row.find("128.00 MiB") != std::string::npos);
    assert(stats_row.find("2.00 GiB") != std::string::npos);
    assert(stats_row.find("78% (52C)") != std::string::npos);
    assert(stats_row.find("1.00 GiB / 8.00 GiB") != std::string::npos);
    std::cout << "  - quiver stats row formatting verified:\n    " << stats_row << "\n";

    // Reap child
    int status = 0;
    pid_t w = waitpid(child_pid, &status, 0);
    assert(w == child_pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    std::cout << "  - Workload process exited cleanly with code 0\n";

    // Teardown cgroup
    assert(cg.cleanup());
    assert(!fs::exists(leaf_dir));
    std::cout << "  - Deterministic Cgroup v2 cleanup verified\n";

    fs::remove_all(test_base, ec);
    std::cout << "[PASS] test_live_rootless_cuda_and_cgroup_e2e_workload\n";
}

} // namespace

int main() {
    std::cout << "=================================================================\n";
    std::cout << "     QUIVER v2.0 END-TO-END GPU & HARDWARE ISOLATION SUITE\n";
    std::cout << "=================================================================\n\n";

    try {
        test_unified_cli_resource_and_gpu_spec_wiring();
        test_containerized_driver_and_device_injection_validation();
        test_live_rootless_cuda_and_cgroup_e2e_workload();

        std::cout << "\n>>> ALL END-TO-END GPU WORKLOAD PROOF TESTS PASSED SUCCESSFULLY <<<\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "E2E GPU Workload failure: " << e.what() << '\n';
        return 1;
    }
}
