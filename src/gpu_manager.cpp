#include "../include/gpu_manager.hpp"
#include "../include/utils.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <format>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>

bool GpuDeviceManager::is_gpu_requested(const ResourceLimits& limits) noexcept {
    return limits.all_gpus || !limits.gpu_indices.empty();
}

std::vector<int> GpuDeviceManager::discover_host_gpu_indices(const fs::path& dev_root) {
    std::vector<int> discovered;
    std::error_code ec;
    if (!fs::exists(dev_root, ec) || !fs::is_directory(dev_root, ec)) {
        return discovered;
    }

    for (const auto& entry : fs::directory_iterator(dev_root, ec)) {
        std::string filename = entry.path().filename().string();
        if (filename.starts_with("nvidia")) {
            std::string suffix = filename.substr(6);
            if (!suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) {
                return std::isdigit(c);
            })) {
                try {
                    discovered.push_back(std::stoi(suffix));
                } catch (...) {}
            }
        }
    }

    std::sort(discovered.begin(), discovered.end());
    discovered.erase(std::unique(discovered.begin(), discovered.end()), discovered.end());
    return discovered;
}

bool GpuDeviceManager::resolve_device_nodes(
    const ResourceLimits& limits,
    const fs::path& dev_root,
    std::vector<std::string>& out_device_names,
    std::string& out_error
) {
    out_device_names.clear();
    out_error.clear();

    if (!is_gpu_requested(limits)) {
        return true;
    }

    std::vector<int> target_indices;
    if (limits.all_gpus) {
        target_indices = discover_host_gpu_indices(dev_root);
        if (target_indices.empty()) {
            out_error = "No NVIDIA GPU devices (/dev/nvidia[0-9]+) found in " + dev_root.string();
            return false;
        }
    } else {
        target_indices = limits.gpu_indices;
        std::error_code ec;
        for (int idx : target_indices) {
            std::string dev_name = "nvidia" + std::to_string(idx);
            fs::path dev_path = dev_root / dev_name;
            if (!fs::exists(dev_path, ec)) {
                out_error = "Requested GPU device node not found: " + dev_path.string();
                return false;
            }
        }
    }

    fs::path ctl_path = dev_root / "nvidiactl";
    std::error_code ec;
    if (!fs::exists(ctl_path, ec)) {
        out_error = "Mandatory control device " + ctl_path.string() + " not found";
        return false;
    }

    for (const char* ctl_dev : CONTROL_DEVICES) {
        if (fs::exists(dev_root / ctl_dev, ec)) {
            out_device_names.emplace_back(ctl_dev);
        }
    }

    for (int idx : target_indices) {
        out_device_names.push_back("nvidia" + std::to_string(idx));
    }

    return true;
}

bool GpuDeviceManager::setup_device_mounts(
    const ResourceLimits& limits,
    const fs::path& old_dev_root,
    const fs::path& new_dev_root,
    std::string& out_error,
    bool dry_run
) {
    std::vector<std::string> device_names;
    if (!resolve_device_nodes(limits, old_dev_root, device_names, out_error)) {
        return false;
    }

    Utils::ensure_dirs(new_dev_root);

    for (const auto& dev_name : device_names) {
        fs::path src = old_dev_root / dev_name;
        fs::path dst = new_dev_root / dev_name;

        Utils::ensure_dirs(dst.parent_path());
        int fd = ::open(dst.c_str(), O_CREAT | O_RDONLY, 0666);
        if (fd < 0) {
            out_error = "Failed to create device mount target " + dst.string() + ": " + std::strerror(errno);
            return false;
        }
        ::close(fd);

        if (!dry_run) {
            if (::mount(src.c_str(), dst.c_str(), nullptr, MS_BIND, nullptr) == -1) {
                out_error = "Failed to bind mount device " + src.string() + " to " + dst.string() + ": " + std::strerror(errno);
                return false;
            }
        }
    }

    fs::path old_caps = old_dev_root / "nvidia-caps";
    std::error_code ec;
    if (fs::exists(old_caps, ec) && fs::is_directory(old_caps, ec)) {
        fs::path new_caps = new_dev_root / "nvidia-caps";
        Utils::ensure_dirs(new_caps);
        for (const auto& entry : fs::directory_iterator(old_caps, ec)) {
            fs::path cap_src = entry.path();
            fs::path cap_dst = new_caps / cap_src.filename();
            int cap_fd = ::open(cap_dst.c_str(), O_CREAT | O_RDONLY, 0666);
            if (cap_fd >= 0) ::close(cap_fd);
            if (!dry_run) {
                ::mount(cap_src.c_str(), cap_dst.c_str(), nullptr, MS_BIND, nullptr);
            }
        }
    }

    return true;
}

std::vector<fs::path> GpuDeviceManager::discover_driver_libraries(const fs::path& host_root) {
    const fs::path candidate_subdirs[] = {
        "usr/lib/x86_64-linux-gnu",
        "usr/lib/aarch64-linux-gnu",
        "usr/lib64",
        "usr/lib",
        "lib/x86_64-linux-gnu",
        "lib/aarch64-linux-gnu",
        "lib64",
        "lib",
        "usr/lib/wsl/lib"
    };

    std::vector<fs::path> discovered;
    std::unordered_set<std::string> seen_canonical_paths;
    std::unordered_set<std::string> seen_sonames;
    std::error_code ec;

    for (const char* essential_lib : ESSENTIAL_DRIVER_LIBS) {
        bool found = false;
        for (const auto& subdir : candidate_subdirs) {
            fs::path dir = host_root / subdir;
            if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
                continue;
            }

            fs::path candidate = dir / essential_lib;
            if (fs::exists(candidate, ec) || fs::is_symlink(candidate, ec)) {
                fs::path current = candidate;
                std::unordered_set<std::string> visited_symlinks;
                while (fs::is_symlink(current, ec)) {
                    std::string cur_str = current.string();
                    if (visited_symlinks.count(cur_str)) break;
                    visited_symlinks.insert(cur_str);

                    fs::path target = fs::read_symlink(current, ec);
                    if (ec) break;
                    if (target.is_absolute()) {
                        current = host_root / target.relative_path();
                    } else {
                        current = current.parent_path() / target;
                    }
                    current = current.lexically_normal();
                }

                if (fs::exists(current, ec)) {
                    std::string key = current.string();
                    if (!seen_canonical_paths.count(key)) {
                        seen_canonical_paths.insert(key);
                        seen_sonames.insert(essential_lib);
                        discovered.push_back(current);
                        found = true;
                        break;
                    }
                }
            }
        }

        if (!found) {
            for (const auto& subdir : candidate_subdirs) {
                fs::path dir = host_root / subdir;
                if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
                    continue;
                }
                for (const auto& entry : fs::directory_iterator(dir, ec)) {
                    std::string fn = entry.path().filename().string();
                    if (fn.starts_with(essential_lib)) {
                        fs::path current = entry.path();
                        std::unordered_set<std::string> visited_symlinks;
                        while (fs::is_symlink(current, ec)) {
                            std::string cur_str = current.string();
                            if (visited_symlinks.count(cur_str)) break;
                            visited_symlinks.insert(cur_str);

                            fs::path target = fs::read_symlink(current, ec);
                            if (ec) break;
                            if (target.is_absolute()) {
                                current = host_root / target.relative_path();
                            } else {
                                current = current.parent_path() / target;
                            }
                            current = current.lexically_normal();
                        }
                        if (fs::exists(current, ec)) {
                            std::string key = current.string();
                            if (!seen_canonical_paths.count(key)) {
                                seen_canonical_paths.insert(key);
                                seen_sonames.insert(essential_lib);
                                discovered.push_back(current);
                                found = true;
                                break;
                            }
                        }
                    }
                }
                if (found) break;
            }
        }
    }

    return discovered;
}

bool GpuDeviceManager::inject_driver_libraries(
    const fs::path& host_root,
    const fs::path& container_root,
    std::string& out_error,
    bool dry_run
) {
    auto libs = discover_driver_libraries(host_root);
    if (libs.empty()) {
        out_error = "No NVIDIA driver libraries discovered under " + host_root.string();
        return false;
    }

    fs::path target_lib_dir = container_root / "usr/lib";
    Utils::ensure_dirs(target_lib_dir);

    for (const auto& src_path : libs) {
        std::string fn = src_path.filename().string();
        std::string soname;
        if (fn.find("libcuda.so") != std::string::npos) {
            soname = "libcuda.so.1";
        } else if (fn.find("libnvidia-ml.so") != std::string::npos) {
            soname = "libnvidia-ml.so.1";
        } else if (fn.find("libnvidia-ptxjitcompiler.so") != std::string::npos) {
            soname = "libnvidia-ptxjitcompiler.so.1";
        } else if (fn.find("libnvidia-fatbinaryloader.so") != std::string::npos) {
            soname = "libnvidia-fatbinaryloader.so";
        } else if (fn.find("libcudadebugger.so") != std::string::npos) {
            soname = "libcudadebugger.so.1";
        } else {
            soname = fn;
        }

        fs::path dst_path = target_lib_dir / soname;
        int fd = ::open(dst_path.c_str(), O_CREAT | O_WRONLY, 0755);
        if (fd >= 0) {
            ::close(fd);
        } else {
            out_error = "Failed to create target library file " + dst_path.string() + ": " + std::strerror(errno);
            return false;
        }

        if (!dry_run) {
            if (::mount(src_path.c_str(), dst_path.c_str(), nullptr, MS_BIND, nullptr) == -1) {
                out_error = "Failed to bind mount " + src_path.string() + " to " + dst_path.string() + ": " + std::strerror(errno);
                return false;
            }

            struct statvfs vfs{};
            int existing_ms_flags = 0;
            if (::statvfs(dst_path.c_str(), &vfs) == 0) {
                if (vfs.f_flag & ST_NOSUID) existing_ms_flags |= MS_NOSUID;
                if (vfs.f_flag & ST_NODEV)  existing_ms_flags |= MS_NODEV;
                if (vfs.f_flag & ST_NOEXEC) existing_ms_flags |= MS_NOEXEC;
                if (vfs.f_flag & ST_NOATIME) existing_ms_flags |= MS_NOATIME;
                if (vfs.f_flag & ST_RELATIME) existing_ms_flags |= MS_RELATIME;
            }
            ::mount(nullptr, dst_path.c_str(), nullptr, MS_BIND | MS_REMOUNT | MS_RDONLY | existing_ms_flags, nullptr);
        }

        std::error_code ec;
        if (soname == "libcuda.so.1") {
            fs::path symlink_path = target_lib_dir / "libcuda.so";
            if (!fs::exists(symlink_path, ec)) {
                ::symlink("libcuda.so.1", symlink_path.c_str());
            }
        } else if (soname == "libnvidia-ml.so.1") {
            fs::path symlink_path = target_lib_dir / "libnvidia-ml.so";
            if (!fs::exists(symlink_path, ec)) {
                ::symlink("libnvidia-ml.so.1", symlink_path.c_str());
            }
        }
    }

    return true;
}

std::string GpuDeviceManager::format_bytes(std::uint64_t bytes) {
    constexpr double KIB = 1024.0;
    constexpr double MIB = 1024.0 * 1024.0;
    constexpr double GIB = 1024.0 * 1024.0 * 1024.0;

    double d = static_cast<double>(bytes);
    if (d >= GIB) {
        return std::format("{:.2f} GiB", d / GIB);
    }
    if (d >= MIB) {
        return std::format("{:.2f} MiB", d / MIB);
    }
    if (d >= KIB) {
        return std::format("{:.2f} KiB", d / KIB);
    }
    return std::format("{} B", bytes);
}

std::string GpuDeviceManager::format_stats_row(
    std::string_view container_id,
    std::string_view container_name,
    double cpu_pct,
    const CgroupMetrics& cg_metrics,
    const std::vector<GpuDeviceTelemetry>& gpu_metrics,
    bool gpu_requested
) {
    std::string cid = container_id.size() > 12 ? std::string(container_id.substr(0, 12)) : std::string(container_id);
    std::string name = std::string(container_name);
    std::string cpu_str = std::format("{:.2f}%", cpu_pct);

    std::string mem_cur = format_bytes(cg_metrics.memory_current_bytes);
    std::string mem_lim = cg_metrics.memory_max_bytes > 0 ? format_bytes(cg_metrics.memory_max_bytes) : "unlimited";
    std::string mem_str = mem_cur + " / " + mem_lim;

    std::string pids_lim = cg_metrics.pids_max > 0 ? std::to_string(cg_metrics.pids_max) : "max";
    std::string pids_str = std::to_string(cg_metrics.pids_current) + " / " + pids_lim;

    std::string gpu_util_str = "N/A";
    std::string gpu_vram_str = "N/A";

    if (gpu_requested && !gpu_metrics.empty()) {
        const auto& g = gpu_metrics[0];
        if (g.temperature_c > 0) {
            gpu_util_str = std::format("{}% ({}C)", g.gpu_utilization_pct, g.temperature_c);
        } else {
            gpu_util_str = std::format("{}%", g.gpu_utilization_pct);
        }
        gpu_vram_str = format_bytes(g.used_vram_bytes) + " / " + format_bytes(g.total_vram_bytes);
    }

    return std::format("{:<14} {:<16} {:<9} {:<23} {:<11} {:<10} {}",
                       cid, name, cpu_str, mem_str, pids_str, gpu_util_str, gpu_vram_str);
}

NvmlTelemetryCollector::NvmlTelemetryCollector() {
    const char* lib_names[] = {
        "libnvidia-ml.so.1",
        "libnvidia-ml.so",
        "/usr/lib/wsl/lib/libnvidia-ml.so.1"
    };

    for (const char* name : lib_names) {
        m_handle = ::dlopen(name, RTLD_LAZY);
        if (m_handle) break;
    }

    if (!m_handle) {
        m_available = false;
        return;
    }

    m_fn_init = reinterpret_cast<nvmlInit_f>(::dlsym(m_handle, "nvmlInit_v2"));
    if (!m_fn_init) {
        m_fn_init = reinterpret_cast<nvmlInit_f>(::dlsym(m_handle, "nvmlInit"));
    }

    m_fn_shutdown = reinterpret_cast<nvmlShutdown_f>(::dlsym(m_handle, "nvmlShutdown"));
    m_fn_get_count = reinterpret_cast<nvmlDeviceGetCount_f>(::dlsym(m_handle, "nvmlDeviceGetCount_v2"));
    if (!m_fn_get_count) {
        m_fn_get_count = reinterpret_cast<nvmlDeviceGetCount_f>(::dlsym(m_handle, "nvmlDeviceGetCount"));
    }

    m_fn_get_handle = reinterpret_cast<nvmlDeviceGetHandleByIndex_f>(::dlsym(m_handle, "nvmlDeviceGetHandleByIndex_v2"));
    if (!m_fn_get_handle) {
        m_fn_get_handle = reinterpret_cast<nvmlDeviceGetHandleByIndex_f>(::dlsym(m_handle, "nvmlDeviceGetHandleByIndex"));
    }

    m_fn_get_name = reinterpret_cast<nvmlDeviceGetName_f>(::dlsym(m_handle, "nvmlDeviceGetName"));
    m_fn_get_memory = reinterpret_cast<nvmlDeviceGetMemoryInfo_f>(::dlsym(m_handle, "nvmlDeviceGetMemoryInfo"));
    m_fn_get_utilization = reinterpret_cast<nvmlDeviceGetUtilizationRates_f>(::dlsym(m_handle, "nvmlDeviceGetUtilizationRates"));
    m_fn_get_temperature = reinterpret_cast<nvmlDeviceGetTemperature_f>(::dlsym(m_handle, "nvmlDeviceGetTemperature"));
    m_fn_get_procs = reinterpret_cast<nvmlDeviceGetComputeRunningProcesses_f>(::dlsym(m_handle, "nvmlDeviceGetComputeRunningProcesses"));

    if (!m_fn_init || !m_fn_shutdown || !m_fn_get_count || !m_fn_get_handle) {
        ::dlclose(m_handle);
        m_handle = nullptr;
        m_available = false;
        return;
    }

    if (m_fn_init() != NVML_SUCCESS) {
        ::dlclose(m_handle);
        m_handle = nullptr;
        m_available = false;
        return;
    }

    unsigned int count = 0;
    if (m_fn_get_count(&count) == NVML_SUCCESS) {
        m_device_count = count;
        m_available = true;
    } else {
        m_fn_shutdown();
        ::dlclose(m_handle);
        m_handle = nullptr;
        m_available = false;
    }
}

NvmlTelemetryCollector::~NvmlTelemetryCollector() {
    if (m_handle) {
        if (m_available && m_fn_shutdown) {
            m_fn_shutdown();
        }
        ::dlclose(m_handle);
        m_handle = nullptr;
    }
}

NvmlTelemetryCollector::NvmlTelemetryCollector(NvmlTelemetryCollector&& other) noexcept
    : m_handle(std::exchange(other.m_handle, nullptr)),
      m_available(std::exchange(other.m_available, false)),
      m_device_count(std::exchange(other.m_device_count, 0)),
      m_fn_init(std::exchange(other.m_fn_init, nullptr)),
      m_fn_shutdown(std::exchange(other.m_fn_shutdown, nullptr)),
      m_fn_get_count(std::exchange(other.m_fn_get_count, nullptr)),
      m_fn_get_handle(std::exchange(other.m_fn_get_handle, nullptr)),
      m_fn_get_name(std::exchange(other.m_fn_get_name, nullptr)),
      m_fn_get_memory(std::exchange(other.m_fn_get_memory, nullptr)),
      m_fn_get_utilization(std::exchange(other.m_fn_get_utilization, nullptr)),
      m_fn_get_temperature(std::exchange(other.m_fn_get_temperature, nullptr)),
      m_fn_get_procs(std::exchange(other.m_fn_get_procs, nullptr)) {}

NvmlTelemetryCollector& NvmlTelemetryCollector::operator=(NvmlTelemetryCollector&& other) noexcept {
    if (this != &other) {
        if (m_handle) {
            if (m_available && m_fn_shutdown) {
                m_fn_shutdown();
            }
            ::dlclose(m_handle);
        }
        m_handle = std::exchange(other.m_handle, nullptr);
        m_available = std::exchange(other.m_available, false);
        m_device_count = std::exchange(other.m_device_count, 0);
        m_fn_init = std::exchange(other.m_fn_init, nullptr);
        m_fn_shutdown = std::exchange(other.m_fn_shutdown, nullptr);
        m_fn_get_count = std::exchange(other.m_fn_get_count, nullptr);
        m_fn_get_handle = std::exchange(other.m_fn_get_handle, nullptr);
        m_fn_get_name = std::exchange(other.m_fn_get_name, nullptr);
        m_fn_get_memory = std::exchange(other.m_fn_get_memory, nullptr);
        m_fn_get_utilization = std::exchange(other.m_fn_get_utilization, nullptr);
        m_fn_get_temperature = std::exchange(other.m_fn_get_temperature, nullptr);
        m_fn_get_procs = std::exchange(other.m_fn_get_procs, nullptr);
    }
    return *this;
}

std::vector<GpuDeviceTelemetry> NvmlTelemetryCollector::collect_metrics(
    const ResourceLimits& limits,
    const std::vector<pid_t>& container_pids
) {
    std::vector<GpuDeviceTelemetry> results;
    if (!m_available || m_device_count == 0) {
        return results;
    }

    std::unordered_set<pid_t> pid_set(container_pids.begin(), container_pids.end());

    std::vector<int> target_indices;
    if (limits.all_gpus) {
        for (unsigned int i = 0; i < m_device_count; ++i) {
            target_indices.push_back(static_cast<int>(i));
        }
    } else {
        for (int idx : limits.gpu_indices) {
            if (idx >= 0 && static_cast<unsigned int>(idx) < m_device_count) {
                target_indices.push_back(idx);
            }
        }
    }

    for (int idx : target_indices) {
        nvmlDevice_t dev = nullptr;
        if (m_fn_get_handle(static_cast<unsigned int>(idx), &dev) != NVML_SUCCESS || !dev) {
            continue;
        }

        GpuDeviceTelemetry t{};
        t.index = idx;

        if (m_fn_get_name) {
            char name_buf[96]{};
            if (m_fn_get_name(dev, name_buf, sizeof(name_buf)) == NVML_SUCCESS) {
                t.name = name_buf;
            }
        }

        if (m_fn_get_memory) {
            nvmlMemory_t mem{};
            if (m_fn_get_memory(dev, &mem) == NVML_SUCCESS) {
                t.total_vram_bytes = mem.total;
                t.used_vram_bytes = mem.used;
            }
        }

        if (m_fn_get_utilization) {
            nvmlUtilization_t util{};
            if (m_fn_get_utilization(dev, &util) == NVML_SUCCESS) {
                t.gpu_utilization_pct = util.gpu;
                t.mem_utilization_pct = util.memory;
            }
        }

        if (m_fn_get_temperature) {
            unsigned int temp = 0;
            if (m_fn_get_temperature(dev, NVML_TEMPERATURE_GPU, &temp) == NVML_SUCCESS) {
                t.temperature_c = temp;
            }
        }

        if (m_fn_get_procs) {
            unsigned int proc_count = 32;
            std::vector<nvmlProcessInfo_v1_t> proc_buf(proc_count);
            nvmlReturn_t ret = m_fn_get_procs(dev, &proc_count, proc_buf.data());
            if (ret == NVML_ERROR_INSUFFICIENT_SIZE && proc_count > 32) {
                proc_buf.resize(proc_count);
                ret = m_fn_get_procs(dev, &proc_count, proc_buf.data());
            }
            if (ret == NVML_SUCCESS) {
                std::uint64_t container_used_vram = 0;
                bool found_container_proc = false;
                for (unsigned int p = 0; p < proc_count; ++p) {
                    GpuProcessSample sample{};
                    sample.pid = proc_buf[p].pid;
                    sample.used_vram_bytes = proc_buf[p].usedGpuMemory;
                    t.processes.push_back(sample);
                    if (pid_set.count(static_cast<pid_t>(sample.pid))) {
                        container_used_vram += sample.used_vram_bytes;
                        found_container_proc = true;
                    }
                }
                if (found_container_proc) {
                    t.used_vram_bytes = container_used_vram;
                }
            }
        }

        results.push_back(t);
    }

    return results;
}

