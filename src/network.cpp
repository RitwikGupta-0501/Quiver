#include "../include/network.hpp"
#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>
#include <cstring>
#include <format>
#include <fcntl.h>

Network::Network(Network&& other) noexcept
    : m_container_pid(other.m_container_pid),
      m_slirp_pid(other.m_slirp_pid),
      m_api_socket(std::move(other.m_api_socket)) {
    other.m_container_pid = -1;
    other.m_slirp_pid = -1;
}

Network& Network::operator=(Network&& other) noexcept {
    if (this != &other) {
        teardown();
        m_container_pid = other.m_container_pid;
        m_slirp_pid = other.m_slirp_pid;
        m_api_socket = std::move(other.m_api_socket);

        other.m_container_pid = -1;
        other.m_slirp_pid = -1;
    }
    return *this;
}

int Network::setup(pid_t container_pid, const std::vector<PortMapping>& ports) {
    m_container_pid = container_pid;
    m_api_socket = get_api_socket_path(container_pid);

    unlink(m_api_socket.c_str());

    if (access("/usr/bin/slirp4netns", X_OK) != 0 && access("/usr/local/bin/slirp4netns", X_OK) != 0) {
        if (!ports.empty()) {
            std::cerr << "Warning: slirp4netns not found, cannot publish ports.\n";
            return -1;
        }
        return 0;
    }

    pid_t slirp_pid = fork();
    if (slirp_pid == 0) {
        if (setsid() == -1) {
            _exit(1);
        }

        int devnull = open("/dev/null", O_RDWR);
        if (devnull != -1) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }

        std::string pid_str = std::to_string(container_pid);
        execlp("slirp4netns", "slirp4netns",
               "--configure",
               "--mtu=65520",
               "--enable-sandbox",
               "--api-socket", m_api_socket.c_str(),
               pid_str.c_str(),
               "tap0",
               (char*)NULL);

        _exit(1);
    } else if (slirp_pid > 0) {
        m_slirp_pid = slirp_pid;
        if (wait_for_api_socket(m_api_socket, 5000) == 0) {
            for (const auto& port : ports) {
                add_port_forward(container_pid, port.host_port, port.container_port);
            }
            return 0;
        } else {
            std::cerr << "Timeout waiting for slirp4netns API socket" << '\n';
            teardown();
            return -1;
        }
    } else {
        std::cerr << "Failed to fork slirp4netns process" << '\n';
        return -1;
    }
}

void Network::teardown() {
    if (m_slirp_pid > 0) {
        kill(m_slirp_pid, SIGTERM);
        int status = 0;
        int result = waitpid(m_slirp_pid, &status, WNOHANG);
        if (result == 0) {
            usleep(100000);
            kill(m_slirp_pid, SIGKILL);
            waitpid(m_slirp_pid, &status, 0);
        }
        m_slirp_pid = -1;
    }
    if (!m_api_socket.empty()) {
        unlink(m_api_socket.c_str());
        m_api_socket.clear();
    }
    m_container_pid = -1;
}

Network::~Network() {
    teardown();
}

int Network::add_port_forward(pid_t container_pid, int host_port, int container_port) {
    std::string json_cmd = std::format(
        "{{\"execute\": \"add_hostfwd\", \"arguments\": {{\"proto\": \"tcp\", \"host_addr\": \"0.0.0.0\", \"host_port\": {}, \"guest_addr\": \"10.0.2.100\", \"guest_port\": {}}}}}",
        host_port, container_port
    );

    if (send_api_command(container_pid, json_cmd) == 0) {
        std::cout << "Port forward added: " << host_port << " -> " << container_port << '\n';
        return 0;
    }
    std::cerr << "Failed to register port forward for PID " << container_pid << '\n';
    return -1;
}

int Network::remove_port_forward(pid_t container_pid, int host_port) {
    std::string json_cmd = std::format(
        "{{\"execute\": \"remove_hostfwd\", \"arguments\": {{\"proto\": \"tcp\", \"host_port\": {}}}}}",
        host_port
    );

    if (send_api_command(container_pid, json_cmd) == 0) {
        std::cout << "Port forward removed: " << host_port << '\n';
        return 0;
    }
    std::cerr << "Failed to remove port forward for PID " << container_pid << '\n';
    return -1;
}

int Network::connect_namespaces(pid_t target_pid, int host_port, int target_port) {
    std::string json_cmd = std::format(
        "{{\"execute\": \"add_hostfwd\", \"arguments\": {{\"proto\": \"tcp\", \"host_addr\": \"127.0.0.1\", \"host_port\": {}, \"guest_addr\": \"10.0.2.100\", \"guest_port\": {}}}}}",
        host_port, target_port
    );

    if (send_api_command(target_pid, json_cmd) == 0) {
        std::cout << "Internal Link Established: Host(127.0.0.1):" << host_port
                  << " -> Container(" << target_pid << "):" << target_port << '\n';
        return 0;
    }
    std::cerr << "Failed to establish internal link for PID " << target_pid << '\n';
    return -1;
}

std::string Network::get_api_socket_path(pid_t container_pid) {
    return "/tmp/slirp4netns-" + std::to_string(container_pid) + ".sock";
}

int Network::send_api_command(pid_t container_pid, const std::string& json_command) {
    std::string socket_path = get_api_socket_path(container_pid);

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        std::cerr << "Failed to create socket" << '\n';
        return -1;
    }

    sockaddr_un addr{};
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        close(sock);
        return -1;
    }

    ssize_t sent = send(sock, json_command.c_str(), json_command.length(), 0);
    if (sent == -1) {
        close(sock);
        return -1;
    }

    char response[1024];
    memset(response, 0, sizeof(response));
    ssize_t received = recv(sock, response, sizeof(response) - 1, 0);
    if (received > 0) {
        response[received] = '\0';
        if (strstr(response, "\"error\"") != nullptr) {
            close(sock);
            return -1;
        }
    } else if (received == -1) {
        close(sock);
        return -1;
    }

    close(sock);
    return 0;
}

int Network::wait_for_api_socket(const std::string& socket_path, int timeout_ms) {
    int elapsed = 0;
    const int check_interval = 50;

    while (elapsed < timeout_ms) {
        if (access(socket_path.c_str(), F_OK) == 0) {
            usleep(100000);
            return 0;
        }
        usleep(check_interval * 1000);
        elapsed += check_interval;
    }

    std::cerr << "Timeout waiting for API socket: " << socket_path << '\n';
    return -1;
}
