#include "cgroup.hpp"
#include "monitor.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <unordered_set>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statfs.h>

#ifndef CGROUP2_SUPER_MAGIC
#define CGROUP2_SUPER_MAGIC 0x63677270
#endif

CgroupManager::CgroupManager(
    std::string container_id,
    ResourceLimits limits,
    std::optional<fs::path> custom_root
) : m_container_id(std::move(container_id)),
    m_limits(std::move(limits)),
    m_custom_root(std::move(custom_root)) {}

CgroupManager::~CgroupManager() {
    if (m_active) {
        cleanup();
    }
}

CgroupManager::CgroupManager(CgroupManager&& other) noexcept
    : m_container_id(std::move(other.m_container_id)),
      m_limits(std::move(other.m_limits)),
      m_custom_root(std::move(other.m_custom_root)),
      m_cgroup_dir(std::move(other.m_cgroup_dir)),
      m_active(other.m_active) {
    other.m_active = false;
    other.m_cgroup_dir.clear();
}

CgroupManager& CgroupManager::operator=(CgroupManager&& other) noexcept {
    if (this != &other) {
        if (m_active) {
            cleanup();
        }
        m_container_id = std::move(other.m_container_id);
        m_limits = std::move(other.m_limits);
        m_custom_root = std::move(other.m_custom_root);
        m_cgroup_dir = std::move(other.m_cgroup_dir);
        m_active = other.m_active;
        other.m_active = false;
        other.m_cgroup_dir.clear();
    }
    return *this;
}

bool CgroupManager::is_cgroup_v2_available(const fs::path& root) noexcept {
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        return false;
    }
    if (fs::exists(root / "cgroup.controllers", ec)) {
        return true;
    }
    struct statfs s{};
    if (::statfs(root.c_str(), &s) == 0) {
        return s.f_type == static_cast<decltype(s.f_type)>(CGROUP2_SUPER_MAGIC);
    }
    return false;
}

std::string CgroupManager::format_memory_max(std::size_t memory_max_bytes) {
    if (memory_max_bytes == 0) {
        return "max\n";
    }
    return std::format("{}\n", memory_max_bytes);
}

std::string CgroupManager::format_cpu_max(double cpus, std::uint64_t period_us) {
    if (cpus <= 0.0) {
        return std::format("max {}\n", period_us);
    }
    std::uint64_t quota_us = static_cast<std::uint64_t>(std::llround(cpus * static_cast<double>(period_us)));
    quota_us = std::max<std::uint64_t>(quota_us, 1000);
    return std::format("{} {}\n", quota_us, period_us);
}

std::string CgroupManager::format_pids_max(int pids_max) {
    if (pids_max <= 0) {
        return "max\n";
    }
    return std::format("{}\n", pids_max);
}

bool CgroupManager::write_cgroup_file(
    const fs::path& file_path,
    std::string_view content,
    std::string& out_error,
    bool is_custom_root
) {
    int flags = O_WRONLY | O_TRUNC;
    if (is_custom_root) {
        flags |= O_CREAT;
    }
    int fd = ::open(file_path.c_str(), flags, 0644);
    if (fd < 0 && is_custom_root) {
        fd = ::open(file_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (fd < 0) {
        out_error = std::format("Failed to open {}: {}", file_path.string(), std::strerror(errno));
        return false;
    }

    ssize_t written = ::write(fd, content.data(), content.size());
    int write_errno = errno;
    ::close(fd);

    if (written < 0 || static_cast<size_t>(written) != content.size()) {
        out_error = std::format("Failed to write to {}: {}", file_path.string(), std::strerror(write_errno));
        return false;
    }
    return true;
}

bool CgroupManager::enable_subtree_controllers(
    const fs::path& parent_dir,
    const std::vector<std::string>& controllers
) {
    fs::path controllers_file = parent_dir / "cgroup.controllers";
    std::unordered_set<std::string> available;
    if (fs::exists(controllers_file)) {
        std::ifstream f(controllers_file);
        std::string name;
        while (f >> name) {
            available.insert(name);
        }
    }

    fs::path subtree_file = parent_dir / "cgroup.subtree_control";
    int fd = ::open(subtree_file.c_str(), O_WRONLY);
    if (fd < 0) {
        return false;
    }

    bool all_succeeded = true;
    for (const auto& ctrl : controllers) {
        if (!available.empty() && !available.contains(ctrl)) {
            continue;
        }
        std::string token = "+" + ctrl;
        ssize_t ret = ::write(fd, token.data(), token.size());
        if (ret < 0) {
            all_succeeded = false;
        }
    }
    ::close(fd);
    return all_succeeded;
}

std::optional<fs::path> CgroupManager::resolve_delegated_parent(std::string& out_error) const {
    // Tier 0: Custom Root Override (Mock / Testing)
    if (m_custom_root.has_value()) {
        return *m_custom_root;
    }

    // Verify cgroup v2 presence
    if (!is_cgroup_v2_available("/sys/fs/cgroup")) {
        out_error = "Unified Cgroup v2 hierarchy not detected at /sys/fs/cgroup";
        return std::nullopt;
    }

    // Tier 1: Direct root / writable /sys/fs/cgroup
    if (::geteuid() == 0 || ::access("/sys/fs/cgroup", W_OK) == 0) {
        fs::path quiver_slice = "/sys/fs/cgroup/quiver";
        std::error_code ec;
        fs::create_directories(quiver_slice, ec);
        if (!ec && ::access(quiver_slice.c_str(), W_OK) == 0) {
            enable_subtree_controllers("/sys/fs/cgroup", {"memory", "cpu", "pids"});
            enable_subtree_controllers(quiver_slice, {"memory", "cpu", "pids"});
            return quiver_slice;
        }
    }

    // Tier 2: Rootless Systemd User Slice Delegation
    std::string rel_path;
    std::ifstream cgroup_file("/proc/self/cgroup");
    if (cgroup_file.is_open()) {
        std::string line;
        while (std::getline(cgroup_file, line)) {
            if (line.starts_with("0::")) {
                rel_path = line.substr(3);
                break;
            }
        }
    }

    while (!rel_path.empty() && rel_path.front() == '/') {
        rel_path.erase(0, 1);
    }
    fs::path current_cg = fs::path("/sys/fs/cgroup") / rel_path;

    uid_t uid = ::getuid();

    // Candidate B: user@<uid>.service / quiver.slice
    fs::path user_service = fs::path("/sys/fs/cgroup/user.slice") /
                            std::format("user-{}.slice", uid) /
                            std::format("user@{}.service", uid);
    if (fs::exists(user_service) && ::access(user_service.c_str(), W_OK) == 0) {
        fs::path quiver_slice = user_service / "quiver.slice";
        std::error_code ec;
        fs::create_directories(quiver_slice, ec);
        if (!ec && ::access(quiver_slice.c_str(), W_OK) == 0) {
            enable_subtree_controllers(user_service, {"memory", "cpu", "pids"});
            enable_subtree_controllers(quiver_slice, {"memory", "cpu", "pids"});
            return quiver_slice;
        }
    }

    // Candidate A: Ascend from current_cg looking for writable ancestor
    for (fs::path p = current_cg.parent_path(); p != "/sys/fs/cgroup" && p != "/" && !p.empty(); p = p.parent_path()) {
        if (::access(p.c_str(), W_OK) == 0) {
            fs::path quiver_slice = p / "quiver.slice";
            std::error_code ec;
            fs::create_directories(quiver_slice, ec);
            if (!ec && ::access(quiver_slice.c_str(), W_OK) == 0) {
                enable_subtree_controllers(p, {"memory", "cpu", "pids"});
                enable_subtree_controllers(quiver_slice, {"memory", "cpu", "pids"});
                return quiver_slice;
            }
        }
    }

    // Candidate C: sub-slice under current_cg if writable
    if (fs::exists(current_cg) && ::access(current_cg.c_str(), W_OK) == 0) {
        fs::path sub_slice = current_cg / "quiver.slice";
        std::error_code ec;
        fs::create_directories(sub_slice, ec);
        if (!ec && ::access(sub_slice.c_str(), W_OK) == 0) {
            enable_subtree_controllers(current_cg, {"memory", "cpu", "pids"});
            enable_subtree_controllers(sub_slice, {"memory", "cpu", "pids"});
            return sub_slice;
        }
    }

    out_error = std::format("No writable cgroup v2 delegated hierarchy found for user {}", uid);
    return std::nullopt;
}

bool CgroupManager::setup(std::string& out_error) {
    auto parent_opt = resolve_delegated_parent(out_error);
    if (!parent_opt) {
        return false;
    }
    fs::path parent_dir = *parent_opt;

    // Ensure controllers enabled in parent_dir
    enable_subtree_controllers(parent_dir, {"memory", "cpu", "pids"});

    std::string short_id = m_container_id.substr(0, std::min<size_t>(12, m_container_id.size()));
    m_cgroup_dir = parent_dir / ("quiver-" + short_id);

    std::error_code ec;
    fs::create_directories(m_cgroup_dir, ec);
    if (ec) {
        out_error = std::format("Failed to create cgroup directory {}: {}", m_cgroup_dir.string(), ec.message());
        return false;
    }

    bool is_mock = m_custom_root.has_value();

    // 1. Memory Limit & Swap
    if (m_limits.memory_max_bytes > 0 || is_mock) {
        std::string mem_content = format_memory_max(m_limits.memory_max_bytes);
        if (!write_cgroup_file(m_cgroup_dir / "memory.max", mem_content, out_error, is_mock)) {
            return false;
        }
        std::string swap_err;
        if (!write_cgroup_file(m_cgroup_dir / "memory.swap.max", "0\n", swap_err, is_mock)) {
            Monitor::get_instance().log("WARN", "Cgroup", "Could not set memory.swap.max: " + swap_err);
        }
    }

    // 2. CPU Bandwidth Quota
    if (m_limits.cpus > 0.0 || is_mock) {
        std::string cpu_content = format_cpu_max(m_limits.cpus);
        if (!write_cgroup_file(m_cgroup_dir / "cpu.max", cpu_content, out_error, is_mock)) {
            return false;
        }
    }

    // 3. PIDs Limit / Fork-Bomb Protection
    std::string pids_content = format_pids_max(m_limits.pids_max);
    if (!write_cgroup_file(m_cgroup_dir / "pids.max", pids_content, out_error, is_mock)) {
        return false;
    }

    m_active = true;
    return true;
}

bool CgroupManager::assign_pid(pid_t pid, std::string& out_error) const {
    if (m_cgroup_dir.empty()) {
        out_error = "Cgroup directory not initialized";
        return false;
    }
    std::string content = std::format("{}\n", pid);
    return write_cgroup_file(m_cgroup_dir / "cgroup.procs", content, out_error, m_custom_root.has_value());
}

bool CgroupManager::cleanup() noexcept {
    if (m_cgroup_dir.empty()) {
        m_active = false;
        return true;
    }

    if (m_custom_root.has_value()) {
        std::error_code ec;
        fs::remove_all(m_cgroup_dir, ec);
        m_active = false;
        return !ec;
    }

    // Real cgroupfs removal
    if (::rmdir(m_cgroup_dir.c_str()) == 0 || errno == ENOENT) {
        m_active = false;
        return true;
    }

    if (errno == EBUSY) {
        fs::path kill_file = m_cgroup_dir / "cgroup.kill";
        int kfd = ::open(kill_file.c_str(), O_WRONLY);
        if (kfd >= 0) {
            ::write(kfd, "1\n", 2);
            ::close(kfd);
        } else {
            fs::path procs_file = m_cgroup_dir / "cgroup.procs";
            std::ifstream procs(procs_file);
            pid_t pid = 0;
            while (procs >> pid) {
                if (pid > 0) {
                    ::kill(pid, SIGKILL);
                }
            }
        }
    }

    // Retry rmdir up to 10 times with 20ms backoff
    for (int retry = 0; retry < 10; ++retry) {
        ::usleep(20000);
        if (::rmdir(m_cgroup_dir.c_str()) == 0 || errno == ENOENT) {
            m_active = false;
            return true;
        }
    }

    m_active = false;
    return false;
}

std::optional<fs::path> CgroupManager::find_container_cgroup_dir(
    std::string_view container_id,
    pid_t container_pid,
    const fs::path& cgroup_root
) {
    std::error_code ec;

    if (container_pid > 0) {
        fs::path proc_cgroup = "/proc/" + std::to_string(container_pid) + "/cgroup";
        if (fs::exists(proc_cgroup, ec)) {
            std::ifstream file(proc_cgroup);
            std::string line;
            while (std::getline(file, line)) {
                if (line.starts_with("0::")) {
                    std::string rel = line.substr(3);
                    while (!rel.empty() && rel.front() == '/') {
                        rel.erase(0, 1);
                    }
                    fs::path candidate = cgroup_root / rel;
                    if (fs::exists(candidate, ec) && fs::is_directory(candidate, ec)) {
                        return candidate.lexically_normal();
                    }
                }
            }
        }
    }

    std::string short_id = container_id.size() > 12 ? std::string(container_id.substr(0, 12)) : std::string(container_id);
    std::string target_name = "quiver-" + short_id;

    fs::path direct = cgroup_root / target_name;
    if (fs::exists(direct, ec) && fs::is_directory(direct, ec)) {
        return direct.lexically_normal();
    }

    fs::path sub = cgroup_root / "quiver" / target_name;
    if (fs::exists(sub, ec) && fs::is_directory(sub, ec)) {
        return sub.lexically_normal();
    }

    if (fs::exists(cgroup_root / std::string(container_id), ec) && fs::is_directory(cgroup_root / std::string(container_id), ec)) {
        return (cgroup_root / std::string(container_id)).lexically_normal();
    }

    if (fs::exists(cgroup_root, ec) && fs::is_directory(cgroup_root, ec)) {
        for (const auto& entry : fs::recursive_directory_iterator(cgroup_root, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            if (entry.is_directory(ec)) {
                std::string fn = entry.path().filename().string();
                if (fn == target_name || fn == container_id) {
                    return entry.path().lexically_normal();
                }
            }
        }
    }

    return std::nullopt;
}

bool CgroupManager::read_metrics(
    const fs::path& cgroup_dir,
    CgroupMetrics& out_metrics
) {
    std::error_code ec;
    if (!fs::exists(cgroup_dir, ec) || !fs::is_directory(cgroup_dir, ec)) {
        return false;
    }

    out_metrics = CgroupMetrics{};

    fs::path mem_cur_path = cgroup_dir / "memory.current";
    if (fs::exists(mem_cur_path, ec)) {
        std::ifstream file(mem_cur_path);
        std::uint64_t val = 0;
        if (file >> val) {
            out_metrics.memory_current_bytes = val;
        }
    }

    fs::path mem_max_path = cgroup_dir / "memory.max";
    if (fs::exists(mem_max_path, ec)) {
        std::ifstream file(mem_max_path);
        std::string str;
        if (file >> str) {
            if (str == "max") {
                out_metrics.memory_max_bytes = 0;
            } else {
                try {
                    out_metrics.memory_max_bytes = std::stoull(str);
                } catch (...) {
                    out_metrics.memory_max_bytes = 0;
                }
            }
        }
    }

    fs::path cpu_stat_path = cgroup_dir / "cpu.stat";
    if (fs::exists(cpu_stat_path, ec)) {
        std::ifstream file(cpu_stat_path);
        std::string line;
        while (std::getline(file, line)) {
            std::istringstream iss(line);
            std::string key;
            std::uint64_t val = 0;
            if (iss >> key >> val) {
                if (key == "usage_usec") {
                    out_metrics.cpu_usage_usec = val;
                } else if (key == "throttled_usec") {
                    out_metrics.cpu_throttled_usec = val;
                }
            }
        }
    }

    fs::path pids_cur_path = cgroup_dir / "pids.current";
    if (fs::exists(pids_cur_path, ec)) {
        std::ifstream file(pids_cur_path);
        std::uint64_t val = 0;
        if (file >> val) {
            out_metrics.pids_current = val;
        }
    }

    fs::path pids_max_path = cgroup_dir / "pids.max";
    if (fs::exists(pids_max_path, ec)) {
        std::ifstream file(pids_max_path);
        std::string str;
        if (file >> str) {
            if (str == "max") {
                out_metrics.pids_max = 0;
            } else {
                try {
                    out_metrics.pids_max = std::stoull(str);
                } catch (...) {
                    out_metrics.pids_max = 0;
                }
            }
        }
    }

    fs::path procs_path = cgroup_dir / "cgroup.procs";
    if (fs::exists(procs_path, ec)) {
        std::ifstream file(procs_path);
        pid_t pid = 0;
        while (file >> pid) {
            if (pid > 0) {
                out_metrics.pids.push_back(pid);
            }
        }
    }

    return true;
}

