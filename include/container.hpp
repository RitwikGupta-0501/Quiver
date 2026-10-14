#pragma once
#include <string>
#include <vector>
#include "state_store.hpp"
#include "terminal.hpp"
#include "network.hpp"

class Container{
    public:
        struct ContainerArgs{
            std::string hostname{};
            std::string rootfs_path{};
            std::string program_path{};
            int slave_fd{};
            std::string filesystem_dir{};
            std::vector<std::string> commands{};
        };

        explicit Container() = default;
        explicit Container(const std::string& container_name,
                           const std::string& hostname,
                           const std::string& new_fs,
                           const std::vector<VolumeMount>& volumes,
                           const std::vector<std::pair<int,int>>& ports,
                           const std::string& container_id,
                           StateStore& store,
                           const std::string& image_name,
                           bool vfs,
                           bool no_remove);
        ~Container() {};
        void exec(const std::string& program_path, const std::vector<std::string>& commands);
        void stop(const pid_t& container_pid, const pid_t& net_pid);
        void set_filesystem(const std::string& path);
        void connect_to_server(const pid_t& container_pid);
        void connect_to_other_container(const pid_t& target_pid, int host_port, int target_port);
    private:
        static void secure_kill(const pid_t& pid);
        void manage_container(const std::string& path, const std::string& filesystem_dir);
        void setup_user_namespace();
        void run(const std::string& path, const std::string& container_id);
        void run_container(const ContainerArgs& args);
        static StateStore* m_store;
        static std::string m_container_id;
        static std::string m_container_name;
        static bool m_vfs;
        static bool m_no_remove;
        static pid_t m_child_pid;
        static std::string m_new_fs;
        static std::string m_new_hostname;
        static std::vector<VolumeMount> m_volumes;
        static std::vector<std::string> m_commands;
        static std::vector<std::pair<int,int>> m_forward_ports;
        static Terminal m_term;
        static Terminal::PtyArgs m_pty_args;
        static std::string m_image_name;
};
