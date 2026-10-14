#pragma once
#include <sys/types.h>
#include <vector>
#include <utility>
#include <string>
#include "state_store.hpp"

class Network {
public:
    Network() = default;
    ~Network();

    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;
    Network(Network&& other) noexcept;
    Network& operator=(Network&& other) noexcept;

    int setup(pid_t container_pid, const std::vector<PortMapping>& ports);
    void teardown();

    [[nodiscard]] pid_t get_net_pid() const noexcept { return m_slirp_pid; }
    [[nodiscard]] const std::string& get_api_socket() const noexcept { return m_api_socket; }

    // Stateless RPC helpers for CLI commands (`quiver network add/rm`)
    static int add_port_forward(pid_t container_pid, int host_port, int container_port);
    static int remove_port_forward(pid_t container_pid, int host_port);
    static int connect_namespaces(pid_t target_pid, int host_port, int target_port);
    static std::string get_api_socket_path(pid_t container_pid);

private:
    static int send_api_command(pid_t container_pid, const std::string& json_command);
    static int wait_for_api_socket(const std::string& socket_path, int timeout_ms = 5000);

    pid_t m_container_pid{-1};
    pid_t m_slirp_pid{-1};
    std::string m_api_socket{};
};
