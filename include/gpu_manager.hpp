#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <sys/types.h>
#include "state_store.hpp"
#include "cgroup.hpp"

namespace fs = std::filesystem;

// Minimal ABI-compatible NVML types
using nvmlReturn_t = int;
constexpr nvmlReturn_t NVML_SUCCESS = 0;
constexpr nvmlReturn_t NVML_ERROR_INSUFFICIENT_SIZE = 7;

using nvmlDevice_t = void*;
constexpr int NVML_TEMPERATURE_GPU = 0;

struct nvmlMemory_t {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

struct nvmlUtilization_t {
    unsigned int gpu;
    unsigned int memory;
};

struct nvmlProcessInfo_v1_t {
    unsigned int pid;
    unsigned long long usedGpuMemory;
};

struct GpuProcessSample {
    unsigned int pid{0};
    std::uint64_t used_vram_bytes{0};
};

struct GpuDeviceTelemetry {
    int index{0};
    std::string name{};
    std::uint64_t total_vram_bytes{0};
    std::uint64_t used_vram_bytes{0};
    unsigned int gpu_utilization_pct{0};
    unsigned int mem_utilization_pct{0};
    unsigned int temperature_c{0};
    std::vector<GpuProcessSample> processes{};
};

class NvmlTelemetryCollector {
public:
    explicit NvmlTelemetryCollector();
    ~NvmlTelemetryCollector();

    NvmlTelemetryCollector(const NvmlTelemetryCollector&) = delete;
    NvmlTelemetryCollector& operator=(const NvmlTelemetryCollector&) = delete;
    NvmlTelemetryCollector(NvmlTelemetryCollector&& other) noexcept;
    NvmlTelemetryCollector& operator=(NvmlTelemetryCollector&& other) noexcept;

    [[nodiscard]] bool is_available() const noexcept { return m_available; }
    [[nodiscard]] unsigned int device_count() const noexcept { return m_device_count; }

    [[nodiscard]] std::vector<GpuDeviceTelemetry> collect_metrics(
        const ResourceLimits& limits,
        const std::vector<pid_t>& container_pids = {}
    );

private:
    void* m_handle{nullptr};
    bool m_available{false};
    unsigned int m_device_count{0};

    using nvmlInit_f = nvmlReturn_t (*)();
    using nvmlShutdown_f = nvmlReturn_t (*)();
    using nvmlDeviceGetCount_f = nvmlReturn_t (*)(unsigned int*);
    using nvmlDeviceGetHandleByIndex_f = nvmlReturn_t (*)(unsigned int, nvmlDevice_t*);
    using nvmlDeviceGetName_f = nvmlReturn_t (*)(nvmlDevice_t, char*, unsigned int);
    using nvmlDeviceGetMemoryInfo_f = nvmlReturn_t (*)(nvmlDevice_t, nvmlMemory_t*);
    using nvmlDeviceGetUtilizationRates_f = nvmlReturn_t (*)(nvmlDevice_t, nvmlUtilization_t*);
    using nvmlDeviceGetTemperature_f = nvmlReturn_t (*)(nvmlDevice_t, int, unsigned int*);
    using nvmlDeviceGetComputeRunningProcesses_f = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*, nvmlProcessInfo_v1_t*);

    nvmlInit_f m_fn_init{nullptr};
    nvmlShutdown_f m_fn_shutdown{nullptr};
    nvmlDeviceGetCount_f m_fn_get_count{nullptr};
    nvmlDeviceGetHandleByIndex_f m_fn_get_handle{nullptr};
    nvmlDeviceGetName_f m_fn_get_name{nullptr};
    nvmlDeviceGetMemoryInfo_f m_fn_get_memory{nullptr};
    nvmlDeviceGetUtilizationRates_f m_fn_get_utilization{nullptr};
    nvmlDeviceGetTemperature_f m_fn_get_temperature{nullptr};
    nvmlDeviceGetComputeRunningProcesses_f m_fn_get_procs{nullptr};
};

class GpuDeviceManager {
public:
    static constexpr const char* CONTROL_DEVICES[] = {
        "nvidiactl",
        "nvidia-uvm",
        "nvidia-uvm-tools",
        "nvidia-modeset"
    };

    static constexpr const char* ESSENTIAL_DRIVER_LIBS[] = {
        "libcuda.so.1",
        "libnvidia-ml.so.1",
        "libnvidia-ptxjitcompiler.so.1",
        "libnvidia-fatbinaryloader.so",
        "libcudadebugger.so.1"
    };

    // 1. Check if GPU passthrough is requested in ResourceLimits
    [[nodiscard]] static bool is_gpu_requested(const ResourceLimits& limits) noexcept;

    // 2. Discover available /dev/nvidia[0-9]+ indices under dev_root (e.g., "/old_root/dev" or "/dev")
    [[nodiscard]] static std::vector<int> discover_host_gpu_indices(
        const fs::path& dev_root = "/dev"
    );

    // 3. Resolve exact device relative paths (e.g., "nvidiactl", "nvidia0") for a given ResourceLimits
    [[nodiscard]] static bool resolve_device_nodes(
        const ResourceLimits& limits,
        const fs::path& dev_root,
        std::vector<std::string>& out_device_names,
        std::string& out_error
    );

    // 4. Bind-mount resolved character devices from old_dev_root into new_dev_root
    static bool setup_device_mounts(
        const ResourceLimits& limits,
        const fs::path& old_dev_root,
        const fs::path& new_dev_root,
        std::string& out_error,
        bool dry_run = false
    );

    // 5. Discover essential host driver libraries under host_root
    [[nodiscard]] static std::vector<fs::path> discover_driver_libraries(
        const fs::path& host_root = "/old_root"
    );

    // 6. Read-only bind-mount discovered libraries into container rootfs (/usr/lib64 or /usr/lib)
    static bool inject_driver_libraries(
        const fs::path& host_root,
        const fs::path& container_root,
        std::string& out_error,
        bool dry_run = false
    );

    // 7. Human-readable bytes formatting helper
    [[nodiscard]] static std::string format_bytes(std::uint64_t bytes);

    // 8. Deterministic telemetry table row formatter
    [[nodiscard]] static std::string format_stats_row(
        std::string_view container_id,
        std::string_view container_name,
        double cpu_pct,
        const CgroupMetrics& cg_metrics,
        const std::vector<GpuDeviceTelemetry>& gpu_metrics,
        bool gpu_requested
    );
};
