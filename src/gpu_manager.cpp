#include "../include/gpu_manager.hpp"
#include "../include/utils.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <unordered_set>

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
