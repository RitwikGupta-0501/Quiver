#pragma once
#include <string>
#include <termios.h>
#include <sys/ioctl.h>

inline constexpr std::size_t SLAVE_NAME_MAX = 128;

class Terminal {
public:
    struct PtyArgs {
        int master_fd{-1};
        int slave_fd{-1};
        char slave_name[SLAVE_NAME_MAX]{};
        winsize window_size{};
    };

    Terminal() = default;
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
    Terminal(Terminal&& other) noexcept;
    Terminal& operator=(Terminal&& other) noexcept;

    void start_pty_session(PtyArgs& args);
    void redirect_io(int slave_fd);
    void start_server(const PtyArgs& args, const std::string& container_id, pid_t container_pid);
    static void connect_to_server(pid_t container_pid);
    void cleanup();
    [[nodiscard]] int get_sfd() const noexcept { return m_sfd; }

private:
    int m_sfd{-1};
    int m_master_fd{-1};
    std::string m_container_id{};
    std::string m_sock_path{};
    pid_t m_container_pid{-1};
    bool m_running{false};
};
