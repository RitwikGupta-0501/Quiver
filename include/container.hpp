#pragma once
#include <string>
#include <vector>
#include <utility>
#include <filesystem>
#include <sys/types.h>
#include "state_store.hpp"
#include "terminal.hpp"
#include "network.hpp"

namespace fs = std::filesystem;

struct ContainerSpec {
    std::string id{};
    std::string name{};
    std::string image{};
    std::string hostname{};
    std::string rootfs_path{};
    std::string working_dir{"/"};
    std::vector<std::string> env{};
    std::vector<std::string> command{};
    std::vector<VolumeMount> volumes{};
    std::vector<PortMapping> ports{};
    ResourceLimits resources{};
    bool use_vfs{false};
    bool no_remove{false};
};

class Container {
public:
    explicit Container(ContainerSpec spec, StateStore& store);
    ~Container() = default;

    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;
    Container(Container&&) noexcept = default;
    Container& operator=(Container&&) noexcept = default;

    void start();
    static void stop(pid_t container_pid, pid_t net_pid);
    static void attach(pid_t container_pid);

private:
    void manage_container(const fs::path& filesystem_dir);
    void run_container(int slave_fd, const fs::path& filesystem_dir);
    static void setup_user_namespace(pid_t target_pid, uid_t host_uid, gid_t host_gid);
    static void secure_kill(pid_t pid);

    ContainerSpec m_spec;
    StateStore& m_store;
    pid_t m_child_pid{-1};
    Terminal m_term{};
    Terminal::PtyArgs m_pty_args{};
    Network m_network{};
};
