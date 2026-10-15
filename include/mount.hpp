#pragma once
#define PROC "proc"
#define DEV "dev"
#define SYS "sysfs"
#define TMPFS "tmpfs"
#define DEVPTS "devpts"
#include "state_store.hpp"
#include <string>
#include <vector>
#include <sys/mount.h>

namespace Mount{
    void proc(const std::string& proc_path, const int& flags, const std::string& options = "");
    void dev(const std::string& dev_path, const int& flags, const std::string& options = "");
    void sys(const std::string& sys_path, const int& flags, const std::string& options = "");
    void tmpfs(const std::string& tmpfs_path, const int& flags, const std::string& options = "");
    void dev_pts(const std::string& dev_pts_path, const int& flags, const std::string& options = "");
    void bind_mount(const std::string& src, const std::string& dst, const std::string& options = "");
    void bind_rec_mount(const std::string& src, const std::string& dst, const std::string& options = "");
    void volumes(const std::string& host_root_prefix, const std::vector<VolumeMount>& volumes, const std::string& options = "");
    void overlay(const fs::path& lower_dir, const fs::path& upper_dir, const fs::path& work_dir, const fs::path& merged_dir);
}
