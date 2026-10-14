#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <sys/types.h>
#include "state_store.hpp"

namespace fs = std::filesystem;

class CgroupManager {
public:
    static constexpr std::uint64_t DEFAULT_CPU_PERIOD_US = 100'000; // 100ms standard CFS period

    explicit CgroupManager(
        std::string container_id,
        ResourceLimits limits,
        std::optional<fs::path> custom_root = std::nullopt
    );
    ~CgroupManager();

    CgroupManager(const CgroupManager&) = delete;
    CgroupManager& operator=(const CgroupManager&) = delete;
    CgroupManager(CgroupManager&& other) noexcept;
    CgroupManager& operator=(CgroupManager&& other) noexcept;

    // 1. Discover writable cgroup v2 parent, create quiver-<id> leaf dir, enable controllers, write limits
    bool setup(std::string& out_error);

    // 2. Write target PID into <cgroup_dir>/cgroup.procs
    bool assign_pid(pid_t pid, std::string& out_error) const;

    // 3. Remove the cgroup leaf directory (retries on EBUSY while processes drain)
    bool cleanup() noexcept;

    // Static formatting helpers (exposed for deterministic unit testing)
    [[nodiscard]] static std::string format_memory_max(std::size_t memory_max_bytes);
    [[nodiscard]] static std::string format_cpu_max(double cpus, std::uint64_t period_us = DEFAULT_CPU_PERIOD_US);
    [[nodiscard]] static std::string format_pids_max(int pids_max);

    // Inspection helpers
    [[nodiscard]] const fs::path& cgroup_path() const noexcept { return m_cgroup_dir; }
    [[nodiscard]] bool is_active() const noexcept { return m_active; }
    [[nodiscard]] static bool is_cgroup_v2_available(const fs::path& root = "/sys/fs/cgroup") noexcept;

private:
    std::optional<fs::path> resolve_delegated_parent(std::string& out_error) const;
    static bool enable_subtree_controllers(const fs::path& parent_dir, const std::vector<std::string>& controllers);
    static bool write_cgroup_file(const fs::path& file_path, std::string_view content, std::string& out_error, bool is_custom_root = false);

    std::string m_container_id{};
    ResourceLimits m_limits{};
    std::optional<fs::path> m_custom_root{};
    fs::path m_cgroup_dir{};
    bool m_active{false};
};
