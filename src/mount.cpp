#include "../include/mount.hpp"
#include "../include/utils.hpp"
#include <format>
#include <cstring>
#include <sys/statvfs.h>

void Mount::proc(const std::string& proc_path, const int& flags, const std::string& options){
    if(mount(PROC, proc_path.c_str(), PROC, flags, options.empty() ? nullptr : options.c_str()) == ERR)
        Utils::handle_error("Unable to mount " + std::string(PROC));
}

void Mount::dev(const std::string& dev_path, const int& flags, const std::string& options){
    if(mount(DEV, dev_path.c_str(), DEV, flags, options.empty() ? nullptr : options.c_str()) == ERR)
        Utils::handle_error("Unable to mount " + std::string(DEV));
}

void Mount::sys(const std::string& sys_path, const int& flags, const std::string& options){
    if(mount(SYS, sys_path.c_str(), SYS, flags, options.empty() ? nullptr : options.c_str()) == ERR)
        Utils::handle_error("Unable to mount " + std::string(SYS));
}

void Mount::tmpfs(const std::string& tmpfs_path, const int& flags, const std::string& options){
    if(mount(TMPFS, tmpfs_path.c_str(), TMPFS, flags, options.empty() ? nullptr : options.c_str()) == ERR)
        Utils::handle_error("Unable to mount " + std::string(TMPFS));
}

void Mount::dev_pts(const std::string& dev_pts_path, const int& flags, const std::string& options){
    if(mount(DEVPTS, dev_pts_path.c_str(), DEVPTS, flags, options.empty() ? nullptr : options.c_str()) == ERR)
        Utils::handle_error("Unable to mount " + std::string(DEVPTS));
}

void Mount::bind_mount(const std::string& src, const std::string& dst, const std::string& options){
    if(mount(src.c_str(), dst.c_str(), nullptr, MS_BIND, options.empty() ? nullptr : options.c_str()) == ERR)
        Utils::handle_error("Unable to bind mount " + src + " to " + dst);
}

void Mount::bind_rec_mount(const std::string &src, const std::string &dst, const std::string& options){
    if(mount(src.c_str(), dst.c_str(), nullptr, MS_BIND | MS_REC, options.empty() ? nullptr : options.c_str()) == ERR)
        Utils::handle_error("Unable to bind and rec mount " + src + " to " + dst);
}

void Mount::volumes(const std::string& host_root_prefix, const std::vector<VolumeMount>& volumes, const std::string& options){
    for (const auto& vol : volumes) {
        std::string src = (fs::path(host_root_prefix) / fs::path(vol.host_path).relative_path()).string();
        std::string dst = vol.container_path;
        Utils::ensure_dirs(dst);
        if (::mount(src.c_str(), dst.c_str(), nullptr, MS_BIND | MS_REC, options.empty() ? nullptr : options.c_str()) == -1) {
            Utils::handle_error("Unable to mount " + src + " to " + dst + ": " + std::strerror(errno));
        }
        if (vol.read_only) {
            struct statvfs vfs{};
            unsigned long existing_ms_flags = 0;
            if (::statvfs(dst.c_str(), &vfs) == 0) {
                if (vfs.f_flag & ST_NOSUID)   existing_ms_flags |= MS_NOSUID;
                if (vfs.f_flag & ST_NODEV)    existing_ms_flags |= MS_NODEV;
                if (vfs.f_flag & ST_NOEXEC)   existing_ms_flags |= MS_NOEXEC;
                if (vfs.f_flag & ST_NOATIME)  existing_ms_flags |= MS_NOATIME;
                if (vfs.f_flag & ST_RELATIME) existing_ms_flags |= MS_RELATIME;
            }
            if (::mount(nullptr, dst.c_str(), nullptr,
                        MS_BIND | MS_REMOUNT | MS_RDONLY | existing_ms_flags, nullptr) == -1) {
                Utils::handle_error("Unable to remount read-only volume " + dst + ": " + std::strerror(errno));
            }
        }
    }
}

void Mount::overlay(
    const fs::path& lower_dir,
    const fs::path& upper_dir,
    const fs::path& work_dir,
    const fs::path& merged_dir
) {
    std::string opts = std::format(
        "lowerdir={},upperdir={},workdir={},userxattr",
        lower_dir.string(), upper_dir.string(), work_dir.string()
    );
    if (::mount("overlay", merged_dir.c_str(), "overlay", MS_NODEV, opts.c_str()) == ERR) {
        Utils::handle_error("Cannot mount rootless overlayfs (" + opts + "): " + std::strerror(errno));
    }
}
