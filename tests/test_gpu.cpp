#include <cassert>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <filesystem>
#include <unistd.h>
#include <sys/stat.h>
#include "gpu_manager.hpp"
#include "cgroup.hpp"
#include "utils.hpp"
#include "state_store.hpp"

namespace fs = std::filesystem;

static void test_gpus_flag_parser() {
    std::cout << "[RUN] test_gpus_flag_parser\n";

    // 1. "all" and case-insensitive "ALL"
    auto all_req = Utils::parse_gpus_string("all");
    assert(all_req.all_gpus == true);
    assert(all_req.gpu_indices.empty());

    auto all_upper = Utils::parse_gpus_string("ALL");
    assert(all_upper.all_gpus == true);
    assert(all_upper.gpu_indices.empty());

    // 2. Single index "0"
    auto single_req = Utils::parse_gpus_string("0");
    assert(single_req.all_gpus == false);
    assert(single_req.gpu_indices == std::vector<int>{0});

    // 3. Deduplication and sorting "2,0,1,0"
    auto multi_req = Utils::parse_gpus_string("2,0,1,0");
    assert(multi_req.all_gpus == false);
    assert((multi_req.gpu_indices == std::vector<int>{0, 1, 2}));

    // 4. Invalid strings throw invalid_argument
    const std::vector<std::string> malformed = {"", "-1", "0,,1", "gpu0", "abc", "0,a", ",0", "0,"};
    for (const auto& bad : malformed) {
        bool threw = false;
        try {
            (void)Utils::parse_gpus_string(bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }

    std::cout << "[PASS] test_gpus_flag_parser\n";
}

static void test_mock_gpu_device_discovery_and_resolution() {
    std::cout << "[RUN] test_mock_gpu_device_discovery_and_resolution\n";

    fs::path mock_dev = fs::temp_directory_path() / ("quiver_gpu_dev_" + std::to_string(::getpid()));
    fs::path mock_cont_dev = fs::temp_directory_path() / ("quiver_cont_dev_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(mock_dev, ec);
    fs::remove_all(mock_cont_dev, ec);
    Utils::ensure_dirs(mock_dev);

    // Create dummy files: nvidiactl, nvidia-uvm, nvidia-uvm-tools, nvidia0, nvidia1, null, zero
    const std::vector<std::string> files = {
        "nvidiactl", "nvidia-uvm", "nvidia-uvm-tools", "nvidia0", "nvidia1", "null", "zero"
    };
    for (const auto& f : files) {
        Utils::write_file(mock_dev / f, "dummy");
    }

    // 1. Discover indices
    auto indices = GpuDeviceManager::discover_host_gpu_indices(mock_dev);
    assert((indices == std::vector<int>{0, 1}));

    // 2. Resolve with all_gpus = true
    ResourceLimits all_limits{};
    all_limits.all_gpus = true;
    std::vector<std::string> out_devices;
    std::string err;
    bool ok = GpuDeviceManager::resolve_device_nodes(all_limits, mock_dev, out_devices, err);
    assert(ok);
    assert((out_devices == std::vector<std::string>{"nvidiactl", "nvidia-uvm", "nvidia-uvm-tools", "nvidia0", "nvidia1"}));

    // 3. Resolve with specific index {1}
    ResourceLimits specific_limits{};
    specific_limits.all_gpus = false;
    specific_limits.gpu_indices = {1};
    out_devices.clear();
    ok = GpuDeviceManager::resolve_device_nodes(specific_limits, mock_dev, out_devices, err);
    assert(ok);
    assert((out_devices == std::vector<std::string>{"nvidiactl", "nvidia-uvm", "nvidia-uvm-tools", "nvidia1"}));

    // 4. Resolve with out-of-range index {3}
    ResourceLimits bad_limits{};
    bad_limits.all_gpus = false;
    bad_limits.gpu_indices = {3};
    out_devices.clear();
    ok = GpuDeviceManager::resolve_device_nodes(bad_limits, mock_dev, out_devices, err);
    assert(!ok);
    assert(!err.empty());

    // 5. setup_device_mounts with dry_run = true
    ok = GpuDeviceManager::setup_device_mounts(specific_limits, mock_dev, mock_cont_dev, err, /*dry_run=*/true);
    assert(ok);
    assert(fs::exists(mock_cont_dev / "nvidiactl"));
    assert(fs::exists(mock_cont_dev / "nvidia1"));
    assert(!fs::exists(mock_cont_dev / "nvidia0"));

    fs::remove_all(mock_dev, ec);
    fs::remove_all(mock_cont_dev, ec);
    std::cout << "[PASS] test_mock_gpu_device_discovery_and_resolution\n";
}

static void test_mock_driver_library_discovery_and_injection() {
    std::cout << "[RUN] test_mock_driver_library_discovery_and_injection\n";

    fs::path mock_old_root = fs::temp_directory_path() / ("quiver_mock_old_root_" + std::to_string(::getpid()));
    fs::path mock_cont_root = fs::temp_directory_path() / ("quiver_mock_cont_root_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(mock_old_root, ec);
    fs::remove_all(mock_cont_root, ec);

    fs::path host_lib_dir = mock_old_root / "usr/lib/x86_64-linux-gnu";
    Utils::ensure_dirs(host_lib_dir);

    // Create real libcuda.so.550.54
    Utils::write_file(host_lib_dir / "libcuda.so.550.54", "fake cuda binary");

    // Create relative symlink libcuda.so.1 -> libcuda.so.550.54
    fs::current_path(host_lib_dir);
    ::symlink("libcuda.so.550.54", (host_lib_dir / "libcuda.so.1").c_str());

    // Create libnvidia-ml.so.1
    Utils::write_file(host_lib_dir / "libnvidia-ml.so.1", "fake nvml binary");

    // Discover libraries
    auto discovered = GpuDeviceManager::discover_driver_libraries(mock_old_root);
    assert(discovered.size() >= 2);

    bool found_cuda = false;
    bool found_nvml = false;
    for (const auto& p : discovered) {
        if (p.filename() == "libcuda.so.550.54") found_cuda = true;
        if (p.filename() == "libnvidia-ml.so.1") found_nvml = true;
    }
    assert(found_cuda);
    assert(found_nvml);

    // Inject libraries with dry_run = true
    std::string err;
    bool injected = GpuDeviceManager::inject_driver_libraries(mock_old_root, mock_cont_root, err, /*dry_run=*/true);
    assert(injected);

    assert(fs::exists(mock_cont_root / "usr/lib/libcuda.so.1"));
    assert(fs::exists(mock_cont_root / "usr/lib/libnvidia-ml.so.1"));
    assert(fs::is_symlink(mock_cont_root / "usr/lib/libcuda.so"));
    assert(fs::is_symlink(mock_cont_root / "usr/lib/libnvidia-ml.so"));

    fs::remove_all(mock_old_root, ec);
    fs::remove_all(mock_cont_root, ec);
    std::cout << "[PASS] test_mock_driver_library_discovery_and_injection\n";
}

static void test_cgroup_live_telemetry_reader() {
    std::cout << "[RUN] test_cgroup_live_telemetry_reader\n";

    fs::path mock_cg = fs::temp_directory_path() / ("quiver_mock_cg_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(mock_cg, ec);
    Utils::ensure_dirs(mock_cg);

    Utils::write_file(mock_cg / "memory.current", "104857600\n");
    Utils::write_file(mock_cg / "memory.max", "536870912\n");
    Utils::write_file(mock_cg / "cpu.stat", "usage_usec 250000\nuser_usec 150000\nsystem_usec 100000\nthrottled_usec 5000\n");
    Utils::write_file(mock_cg / "pids.current", "7\n");
    Utils::write_file(mock_cg / "pids.max", "512\n");
    Utils::write_file(mock_cg / "cgroup.procs", "1001\n1002\n");

    CgroupMetrics metrics{};
    bool ok = CgroupManager::read_metrics(mock_cg, metrics);
    assert(ok);
    assert(metrics.memory_current_bytes == 104857600ULL);
    assert(metrics.memory_max_bytes == 536870912ULL);
    assert(metrics.cpu_usage_usec == 250000ULL);
    assert(metrics.cpu_throttled_usec == 5000ULL);
    assert(metrics.pids_current == 7ULL);
    assert(metrics.pids_max == 512ULL);
    assert(metrics.pids.size() == 2);
    assert(metrics.pids[0] == 1001);
    assert(metrics.pids[1] == 1002);

    fs::remove_all(mock_cg, ec);
    std::cout << "[PASS] test_cgroup_live_telemetry_reader\n";
}

static void test_state_store_gpu_persistence() {
    std::cout << "[RUN] test_state_store_gpu_persistence\n";

    fs::path test_base = fs::temp_directory_path() / ("quiver_store_gpu_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(test_base, ec);

    {
        StateStore store(test_base.string());
        ContainerState cs{};
        cs.id = "gpu-container-123456";
        cs.name = "test-gpu-node";
        cs.image = "nvidia/cuda:12.0-base";
        cs.status = "created";
        cs.resources.all_gpus = false;
        cs.resources.gpu_indices = {0, 2};

        bool saved = store.save_container(cs);
        assert(saved);

        auto loaded = store.get_container("gpu-container-123456");
        assert(loaded.has_value());
        assert(loaded->id == "gpu-container-123456");
        assert(loaded->resources.all_gpus == false);
        assert((loaded->resources.gpu_indices == std::vector<int>{0, 2}));
    }

    fs::remove_all(test_base, ec);
    std::cout << "[PASS] test_state_store_gpu_persistence\n";
}

static void test_nvml_graceful_runtime_loader_and_telemetry_formatting() {
    std::cout << "[RUN] test_nvml_graceful_runtime_loader_and_telemetry_formatting\n";

    // 1. Dynamic loader robustness (does not crash whether driver present or absent)
    NvmlTelemetryCollector collector;
    ResourceLimits limits{};
    limits.all_gpus = true;
    auto gpu_metrics = collector.collect_metrics(limits);
    (void)gpu_metrics;

    // 2. format_bytes testing
    assert(GpuDeviceManager::format_bytes(500) == "500 B");
    assert(GpuDeviceManager::format_bytes(1048576ULL * 100ULL) == "100.00 MiB");
    assert(GpuDeviceManager::format_bytes(1073741824ULL * 2ULL) == "2.00 GiB");

    // 3. format_stats_row testing with synthetic metrics
    CgroupMetrics cm{};
    cm.memory_current_bytes = 44145049ULL; // ~42.10 MiB
    cm.memory_max_bytes = 536870912ULL;    // 512.00 MiB
    cm.pids_current = 4;
    cm.pids_max = 512;

    GpuDeviceTelemetry gt{};
    gt.index = 0;
    gt.gpu_utilization_pct = 35;
    gt.temperature_c = 48;
    gt.used_vram_bytes = 1288490188ULL;  // ~1.20 GiB
    gt.total_vram_bytes = 8589934592ULL; // 8.00 GiB

    std::string row_with_gpu = GpuDeviceManager::format_stats_row(
        "a1b2c3d4e5f6", "quiver-a1b2c3d4", 14.25, cm, {gt}, /*gpu_requested=*/true
    );
    assert(row_with_gpu.find("a1b2c3d4e5f6") != std::string::npos);
    assert(row_with_gpu.find("14.25%") != std::string::npos);
    assert(row_with_gpu.find("42.10 MiB") != std::string::npos);
    assert(row_with_gpu.find("512.00 MiB") != std::string::npos);
    assert(row_with_gpu.find("4 / 512") != std::string::npos);
    assert(row_with_gpu.find("35% (48C)") != std::string::npos);
    assert(row_with_gpu.find("1.20 GiB / 8.00 GiB") != std::string::npos);

    std::string row_no_gpu = GpuDeviceManager::format_stats_row(
        "a1b2c3d4e5f6", "quiver-a1b2c3d4", 0.0, cm, {}, /*gpu_requested=*/false
    );
    assert(row_no_gpu.find("N/A") != std::string::npos);

    // 4. Multi-GPU format_stats_row testing (Sub-Phase 6F regression assertion)
    GpuDeviceTelemetry gt0{};
    gt0.index = 0;
    gt0.gpu_utilization_pct = 40;
    gt0.temperature_c = 50;
    gt0.used_vram_bytes = 1073741824ULL;  // 1.00 GiB
    gt0.total_vram_bytes = 8589934592ULL; // 8.00 GiB

    GpuDeviceTelemetry gt1{};
    gt1.index = 1;
    gt1.gpu_utilization_pct = 80;
    gt1.temperature_c = 65;
    gt1.used_vram_bytes = 3221225472ULL;  // 3.00 GiB
    gt1.total_vram_bytes = 8589934592ULL; // 8.00 GiB

    std::string row_multi_gpu = GpuDeviceManager::format_stats_row(
        "b2c3d4e5f6a1", "quiver-multi-gpu", 28.5, cm, {gt0, gt1}, /*gpu_requested=*/true
    );
    assert(row_multi_gpu.find("60%") != std::string::npos);
    assert(row_multi_gpu.find("4.00 GiB / 16.00 GiB") != std::string::npos);
    assert(row_multi_gpu.find("(65C)") != std::string::npos);

    std::cout << "[PASS] test_nvml_graceful_runtime_loader_and_telemetry_formatting\n";
}

int main() {
    try {
        test_gpus_flag_parser();
        test_mock_gpu_device_discovery_and_resolution();
        test_mock_driver_library_discovery_and_injection();
        test_cgroup_live_telemetry_reader();
        test_state_store_gpu_persistence();
        test_nvml_graceful_runtime_loader_and_telemetry_formatting();

        std::cout << "\n>>> ALL 6 GPU & TELEMETRY TEST SUITES PASSED SUCCESSFULLY <<<\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failure: " << e.what() << '\n';
        return 1;
    }
}
