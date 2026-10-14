#include "../include/mount.hpp"
#include "../include/utils.hpp"

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
        Utils::handle_error("Unable to mount " + std::string(PROC));
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

void Mount::volumes(const std::string& rootfs, const std::vector<VolumeMount>& volumes, const std::string& options){
    size_t no_volumes{ volumes.size() };
    if(no_volumes > 0){
        std::vector<std::string> paths{};
        std::vector<std::string> dirs{};
        for(size_t i{0};i<no_volumes;++i){
            paths.emplace_back(volumes[i].host_path);
            dirs.emplace_back(volumes[i].container_path);
        }
        for(size_t i{0};i<dirs.size();++i){
            dirs[i] = rootfs + dirs[i];
            Utils::ensure_dirs(dirs[i]);
        }
        for(size_t i{0};i<paths.size();++i){
            if(mount(paths[i].c_str(), dirs[i].c_str(), nullptr, MS_BIND|MS_REC, options.empty() ? nullptr : options.c_str()) == -1){
                Utils::handle_error("Unable to mount " + paths[i] + " to " + dirs[i]);
            }
        }
    }
}
