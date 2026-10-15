#include "../include/terminal.hpp"
#include "../include/utils.hpp"
#include <cstdlib>
#include <pty.h>
#include <termios.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>

Terminal::Terminal(Terminal&& other) noexcept
    : m_sfd(other.m_sfd),
      m_master_fd(other.m_master_fd),
      m_container_id(std::move(other.m_container_id)),
      m_sock_path(std::move(other.m_sock_path)),
      m_container_pid(other.m_container_pid),
      m_running(other.m_running) {
    other.m_sfd = -1;
    other.m_master_fd = -1;
    other.m_container_pid = -1;
    other.m_running = false;
}

Terminal& Terminal::operator=(Terminal&& other) noexcept {
    if (this != &other) {
        cleanup();
        m_sfd = other.m_sfd;
        m_master_fd = other.m_master_fd;
        m_container_id = std::move(other.m_container_id);
        m_sock_path = std::move(other.m_sock_path);
        m_container_pid = other.m_container_pid;
        m_running = other.m_running;

        other.m_sfd = -1;
        other.m_master_fd = -1;
        other.m_container_pid = -1;
        other.m_running = false;
    }
    return *this;
}

void Terminal::start_pty_session(PtyArgs& args) {
    if (openpty(&args.master_fd, &args.slave_fd, args.slave_name, nullptr, &args.window_size) == ERR)
        Utils::handle_error("Unable to open a new pty session");
}

void Terminal::redirect_io(int slave_fd) {
    if (ioctl(slave_fd, TIOCSCTTY, 0) == ERR)
        Utils::handle_error("ioctl TCIOCSCTTY");
    if (dup2(slave_fd, STDIN_FILENO) == ERR)
        Utils::handle_error("Redirect stdin to slave file descriptor");
    if (dup2(slave_fd, STDOUT_FILENO) == ERR)
        Utils::handle_error("Redirect stdout to slave file descriptor");
    if (dup2(slave_fd, STDERR_FILENO) == ERR)
        Utils::handle_error("Redirect stderr to slave file descriptor");
}

void Terminal::start_server(const PtyArgs& args, const std::string& container_id, pid_t container_pid) {
    m_container_id = container_id;
    m_container_pid = container_pid;
    m_master_fd = args.master_fd;
    m_sock_path = Utils::get_sock_path(container_pid);

    unlink(m_sock_path.c_str());

    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd == ERR) Utils::handle_error("TTY Proxy Socket Creation failed");
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, m_sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(sfd, (sockaddr*)&addr, sizeof(addr)) == ERR) {
        close(sfd);
        Utils::handle_error("TTY Proxy Socket binding failed");
    }

    chmod(m_sock_path.c_str(), S_IRUSR | S_IWUSR);

    if (!container_id.empty()) {
        std::string id_sock = Utils::get_sock_path(container_id);
        unlink(id_sock.c_str());
        std::error_code ec;
        fs::create_symlink(m_sock_path, id_sock, ec);
    }

    if (listen(sfd, 1) == ERR) {
        close(sfd);
        unlink(m_sock_path.c_str());
        Utils::handle_error("TTY Proxy listen failed");
    }

    m_running = true;
    m_sfd = sfd;

    namespace fs = std::filesystem;
    fs::path log_path = fs::path(Utils::get_base_dir()) / "containers" / container_id / "stdout.log";
    std::error_code ec;
    fs::create_directories(log_path.parent_path(), ec);
    int log_fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);

    while (true) {
        siginfo_t info{};
        if (waitid(P_PID, container_pid, &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == container_pid) {
            break;
        }

        pollfd pf[2]{};
        pf[0].fd = sfd;
        pf[0].events = POLLIN;
        pf[1].fd = args.master_fd;
        pf[1].events = POLLIN;
        int rv = poll(pf, 2, 500);

        if (rv < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rv == 0) {
            continue;
        }

        if (pf[1].revents & POLLIN) {
            char buf[4096]{};
            ssize_t n = read(args.master_fd, buf, sizeof(buf));
            if (n > 0) {
                if (log_fd >= 0) {
                    [[maybe_unused]] auto _w = ::write(log_fd, buf, n);
                }
            } else if (n <= 0) {
                m_running = false;
                break;
            }
        }
        if (pf[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            m_running = false;
            break;
        }

        if (!(pf[0].revents & POLLIN)) {
            continue;
        }

        int cfd = accept(sfd, NULL, NULL);
        if (cfd == ERR) {
            if (errno == EINTR) continue;
            break;
        }

        bool client_connected = true;
        while (client_connected) {
            siginfo_t info_c{};
            if (waitid(P_PID, container_pid, &info_c, WEXITED | WNOHANG | WNOWAIT) == 0 && info_c.si_pid == container_pid) {
                client_connected = false;
                m_running = false;
                break;
            }

            pollfd pf2[2]{};
            pf2[0].fd = args.master_fd;
            pf2[0].events = POLLIN;
            pf2[1].fd = cfd;
            pf2[1].events = POLLIN;

            int r2 = poll(pf2, 2, 500);
            if (r2 < 0) {
                if (errno == EINTR) continue;
                client_connected = false;
                break;
            }
            if (r2 == 0) continue;

            if ((pf2[0].revents | pf2[1].revents) & (POLLERR | POLLHUP | POLLNVAL)) {
                if (pf2[0].revents & (POLLHUP | POLLERR)) {
                    m_running = false;
                }
                client_connected = false;
                break;
            }

            if (pf2[0].revents & POLLIN) {
                char buf[4096]{};
                ssize_t n = read(args.master_fd, buf, sizeof(buf));
                if (n <= 0) {
                    m_running = false;
                    client_connected = false;
                } else {
                    if (log_fd >= 0) {
                        [[maybe_unused]] auto _w = ::write(log_fd, buf, n);
                    }
                    if (write(cfd, buf, n) <= 0) {
                        client_connected = false;
                    }
                }
            }

            if (pf2[1].revents & POLLIN) {
                char buf[4096]{};
                ssize_t n = read(cfd, buf, sizeof(buf));
                if (n <= 0) {
                    client_connected = false;
                } else if (write(args.master_fd, buf, n) <= 0) {
                    m_running = false;
                    client_connected = false;
                }
            }
        }
        close(cfd);

        if (!m_running) {
            break;
        }
    }
    if (log_fd >= 0) {
        ::close(log_fd);
    }
    cleanup();
}

void Terminal::connect_to_server(pid_t container_pid) {
    std::string sock_path = Utils::get_sock_path(container_pid);
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd == ERR) return Utils::handle_error("Attach Socket failed");

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path.data(), sizeof(addr.sun_path) - 1);

    if (connect(sfd, (sockaddr*)&addr, sizeof(addr)) == ERR) {
        close(sfd);
        return Utils::handle_error("Attach Socket Connection failed");
    }

    termios oldt{};
    if (tcgetattr(STDIN_FILENO, &oldt) == ERR) {
        close(sfd);
        return Utils::handle_error("Attach tcgetattr failed");
    }

    termios newt = oldt;
    cfmakeraw(&newt);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    bool saw_ctrlp = false;
    bool should_run = true;
    bool user_detached = false;

    while (should_run) {
        pollfd fds[2]{};
        fds[0].fd = STDIN_FILENO;
        fds[0].events = POLLIN;
        fds[1].fd = sfd;
        fds[1].events = POLLIN;

        int rc = poll(fds, 2, ERR);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if ((fds[0].revents | fds[1].revents) & (POLLERR | POLLHUP | POLLNVAL)) {
            break;
        }

        if (fds[0].revents & POLLIN) {
            char buf[4096];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) {
                should_run = false;
                break;
            }

            for (ssize_t i = 0; i < n; ++i) {
                unsigned char c = static_cast<unsigned char>(buf[i]);

                if (saw_ctrlp) {
                    if (c == 0x11) {
                        should_run = false;
                        user_detached = true;
                        break;
                    }
                    unsigned char seq[2]{ 0x10, c };
                    if (write(sfd, seq, 2) <= 0) {
                        should_run = false;
                        break;
                    }
                    saw_ctrlp = false;
                } else if (c == 0x10) {
                    saw_ctrlp = true;
                } else {
                    if (write(sfd, &c, 1) <= 0) {
                        should_run = false;
                        break;
                    }
                }
            }
        }

        if (!should_run) break;

        if (fds[1].revents & POLLIN) {
            char buf[4096];
            ssize_t n = read(sfd, buf, sizeof(buf));
            if (n <= 0) {
                should_run = false;
                break;
            }
            if (write(STDOUT_FILENO, buf, n) <= 0) {
                should_run = false;
                break;
            }
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    close(sfd);

    if (user_detached) {
        fprintf(stderr, "\r\n[detached from container]\r\n");
    } else {
        fprintf(stderr, "\r\n[session terminated by server]\r\n");
    }
}

void Terminal::cleanup() {
    if (m_sfd >= 0) {
        ::close(m_sfd);
        m_sfd = -1;
    }
    if (m_master_fd >= 0) {
        ::close(m_master_fd);
        m_master_fd = -1;
    }
    if (!m_sock_path.empty()) {
        ::unlink(m_sock_path.c_str());
        m_sock_path.clear();
    }
    if (!m_container_id.empty()) {
        std::string id_sock = Utils::get_sock_path(m_container_id);
        ::unlink(id_sock.c_str());
    }
    m_running = false;
}

Terminal::~Terminal() {
    cleanup();
}
