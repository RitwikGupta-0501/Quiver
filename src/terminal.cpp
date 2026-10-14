#include "../include/terminal.hpp"
#include "../include/utils.hpp"
#include "../include/network.hpp"
#include "../include/container.hpp"
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

int Terminal::m_sfd{-1};
std::string Terminal::m_container_id{""};
termios Terminal::m_orig_term{};
pid_t Terminal::m_container_pid{-1};
volatile bool Terminal::m_running{ false };

void Terminal::start_pty_session(PtyArgs& args){
    if(openpty(&args.master_fd, &args.slave_fd, args.slave_name, nullptr, &args.window_size) == ERR)
        Utils::handle_error("Unable to open a new pty session");
}

void Terminal::redirect_io(const int &slave_fd){
    if(ioctl(slave_fd, TIOCSCTTY, 0) == ERR)
        Utils::handle_error("ioctl TCIOCSCTTY");
    if(dup2(slave_fd, STDIN_FILENO) == ERR)
        Utils::handle_error("Redirect stdin to slave file descriptor");
    if(dup2(slave_fd, STDOUT_FILENO) == ERR)
        Utils::handle_error("Redirect stdout to slave file descriptor");
    if(dup2(slave_fd, STDERR_FILENO) == ERR)
        Utils::handle_error("Redirect stderr to slave file descriptor");
}

int Terminal::get_sfd() const {
    return m_sfd;
}
void Terminal::start_server(const PtyArgs& args, const std::string& container_id, const pid_t& container_pid){
    m_container_id = container_id;
    std::string sock_path{ Utils::get_sock_path(container_pid) };

    unlink(sock_path.c_str());

    int sfd{ socket(AF_UNIX, SOCK_STREAM, 0) };
    if (sfd == ERR) Utils::handle_error("TTY Proxy Socket Creation failed");
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(sfd, (sockaddr*)&addr, sizeof(addr)) == ERR) {
        close(sfd);
        Utils::handle_error("TTY Proxy Socket binding failed");
    }

    chmod(sock_path.c_str(), S_IRUSR | S_IWUSR);

    if (listen(sfd, 1) == ERR) {
        close(sfd);
        unlink(sock_path.c_str());
        Utils::handle_error("TTY Proxy listen failed");
    }

    m_running = true;
    m_sfd = sfd;

    while (true) {
        int status{};
        pid_t r{ waitpid(container_pid, &status, WNOHANG) };
        if (r == container_pid) {
            break;
        }

        pollfd pf{};
        pf.fd = sfd;
        pf.events = POLLIN;
        int rv{ poll(&pf, 1, 500) };

        if (rv < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rv == 0) {
            continue;
        }

        int cfd{ accept(sfd, NULL, NULL) };
        if (cfd == ERR) {
            if (errno == EINTR) continue;
            break;
        }

        bool client_connected { true };
        while (client_connected) {
            pid_t rr{ waitpid(container_pid, &status, WNOHANG) };
            if (rr == container_pid) {
                client_connected = false;
                m_running = false;
                break;
            }

            pollfd pf2[2]{};
            pf2[0].fd = args.master_fd;
            pf2[0].events = POLLIN;
            pf2[1].fd = cfd;
            pf2[1].events = POLLIN;

            int r2{ poll(pf2, 2, 500) };
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
                ssize_t n{ read(args.master_fd, buf, sizeof(buf)) };
                if (n <= 0) {
                    m_running = false;
                    client_connected = false;
                } else if (write(cfd, buf, n) <= 0) {
                    client_connected = false;
                }
            }

            if (pf2[1].revents & POLLIN) {
                char buf[4096]{};
                ssize_t n{ read(cfd, buf, sizeof(buf)) };
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
    cleanup(sfd, args.master_fd);
}

void Terminal::connect_to_server(const int& container_pid){
    std::string sock_path{ Utils::get_sock_path(container_pid) };
    int sfd{ socket(AF_UNIX, SOCK_STREAM, 0) };
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

    termios newt { oldt };
    cfmakeraw(&newt);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    bool saw_ctrlp { false };
    bool should_run { true };
    bool user_detached { false };

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
                    unsigned char seq[2] { 0x10, c };
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
            ssize_t n { read(sfd, buf, sizeof(buf)) };
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
    }
    else {
        fprintf(stderr, "\r\n[session terminated by server]\r\n");
    }
}

void Terminal::cleanup(int sfd, int master_fd){
    if (sfd >= 0) close(sfd);
    if (master_fd >= 0) close(master_fd);
    m_running = false;
}
Terminal::~Terminal(){
}
