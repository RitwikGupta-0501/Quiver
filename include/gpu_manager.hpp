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

namespace fs = std::filesystem;

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
};
