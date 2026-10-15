#include "../include/container.hpp"
#include "../include/utils.hpp"
#include "../include/network.hpp"
#include "../include/mount.hpp"
#include "../include/device_manager.hpp"
#include "../include/monitor.hpp"
#include "../include/cgroup.hpp"
#include "../include/gpu_manager.hpp"
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <cstring>
#include <format>
#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>

Container::Container(ContainerSpec spec, StateStore& store)
    : m_spec(std::move(spec)), m_store(store) {}

void Container::setup_user_namespace(pid_t target_pid, uid_t host_uid, gid_t host_gid) {
    std::string pid_str = std::to_string(target_pid);
    std::string uid_map = std::format("0 {} 1\n", host_uid);
    std::string gid_map = std::format("0 {} 1\n", host_gid);

    Utils::write_file("/proc/" + pid_str + "/setgroups", "deny\n");
    Utils::write_file("/proc/" + pid_str + "/gid_map", gid_map);
    Utils::write_file("/proc/" + pid_str + "/uid_map", uid_map);
}

void Container::secure_kill(pid_t pid) {
    if (pid <= 0) return;

    if (kill(pid, 0) != 0) {
        if (errno == ESRCH) return;
    }

    kill(pid, SIGTERM);

    for (int i = 0; i < 10; ++i) {
        usleep(100000);
        if (kill(pid, 0) != 0) {
            if (errno == ESRCH) return;
        }
    }
    kill(pid, SIGKILL);
}

void Container::stop(pid_t container_pid, pid_t net_pid) {
    secure_kill(container_pid);
    if (net_pid > 0) {
        secure_kill(net_pid);
    }
    std::string api_socket = Network::get_api_socket_path(container_pid);
    unlink(api_socket.c_str());
}

void Container::attach(pid_t container_pid) {
    Terminal::connect_to_server(container_pid);
}

void Container::start() {
    fs::path bundle_dir = m_store.container_bundle_dir(m_spec.id);
    fs::path rootfs_dir = bundle_dir / "rootfs";
    Utils::ensure_dirs(rootfs_dir);

    ioctl(STDIN_FILENO, TIOCGWINSZ, &m_pty_args.window_size);
    m_term.start_pty_session(m_pty_args);

    pid_t manager_pid = fork();
    if (manager_pid == ERR) {
        Utils::handle_error("fork for manager failed");
    }

    if (manager_pid == 0) {
        manage_container(rootfs_dir);
    } else {
        close(m_pty_args.master_fd);
        close(m_pty_args.slave_fd);

        std::cout << "Container started." << '\n';
        std::cout << "To attach, run: quiver attach " << m_spec.id << '\n';

        int status = 0;
        waitpid(manager_pid, &status, 0);
    }
}

void Container::manage_container(const fs::path& filesystem_dir) {
    if (fork() != 0) {
        _exit(0);
    }
    if (setsid() == -1) {
        Utils::handle_error("setsid failed for manager");
    }

    Monitor::get_instance().set_log_path(Utils::get_logs_path(m_spec.id));
    Monitor::get_instance().log("INFO", "Container", "Manager daemon started for container ID: " + m_spec.id);

    uid_t host_uid = ::getuid();
    gid_t host_gid = ::getgid();

    int parent_to_child_pipe[2];
    int child_to_parent_pipe[2];
    if (pipe(parent_to_child_pipe) == ERR || pipe(child_to_parent_pipe) == ERR) {
        Utils::handle_error("pipe creation failed");
    }

    m_child_pid = fork();
    if (m_child_pid == ERR) {
        Utils::handle_error("fork failed");
    }

    if (m_child_pid == 0) {
        // Child process: immediately close the PTY master FD so it never leaks into the container
        if (m_pty_args.master_fd >= 0) {
            ::close(m_pty_args.master_fd);
            m_pty_args.master_fd = -1;
        }
        close(parent_to_child_pipe[1]);
        close(child_to_parent_pipe[0]);

        if (unshare(CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWNS) != 0) {
            Utils::handle_error("unshare failed: " + std::string(std::strerror(errno)));
        }

        char ready_signal = '1';
        if (write(child_to_parent_pipe[1], &ready_signal, 1) != 1) {
            Utils::handle_error("could not signal parent");
        }
        close(child_to_parent_pipe[1]);

        char sync_signal = 0;
        if (read(parent_to_child_pipe[0], &sync_signal, 1) != 1) {
            Utils::handle_error("Failed to receive sync from parent");
        }
        close(parent_to_child_pipe[0]);

        pid_t parent_pid = ::getpid();
        pid_t container_init = fork();
        if (container_init == ERR) {
            Utils::handle_error("fork for PID namespace failed");
        }

        if (container_init == 0) {
            ::prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (::getppid() != parent_pid) {
                _exit(1);
            }
            run_container(m_pty_args.slave_fd, filesystem_dir);
        } else {
            close(m_pty_args.slave_fd);

            int status = 0;
            waitpid(container_init, &status, 0);
            _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
        }
    } else {
        // Manager parent process: immediately close the PTY slave FD so master_fd receives POLLHUP on container exit
        if (m_pty_args.slave_fd >= 0) {
            ::close(m_pty_args.slave_fd);
            m_pty_args.slave_fd = -1;
        }
        close(parent_to_child_pipe[0]);
        close(child_to_parent_pipe[1]);

        char ready_signal = 0;
        if (read(child_to_parent_pipe[0], &ready_signal, 1) != 1) {
            Utils::handle_error("could not read signal from child");
        }
        close(child_to_parent_pipe[0]);

        setup_user_namespace(m_child_pid, host_uid, host_gid);

        // Phase 3: Setup Cgroup v2 limits and enroll m_child_pid BEFORE waking child
        CgroupManager cgroup(m_spec.id, m_spec.resources);
        std::string cg_err;
        if (cgroup.setup(cg_err)) {
            if (!cgroup.assign_pid(m_child_pid, cg_err)) {
                Monitor::get_instance().log("WARN", "Cgroup", "Failed to assign PID " + std::to_string(m_child_pid) + " to cgroup: " + cg_err);
            } else {
                Monitor::get_instance().log("INFO", "Cgroup", "Applied cgroup v2 limits at " + cgroup.cgroup_path().string());
            }
        } else {
            Monitor::get_instance().log("WARN", "Cgroup", "Cgroup v2 delegation unavailable (continuing unthrottled): " + cg_err);
        }

        if (m_network.setup(m_child_pid, m_spec.ports) != 0) {
            Monitor::get_instance().log("ERROR", "Network", "Failed to setup networking for PID " + std::to_string(m_child_pid));
        }

        // Now release m_child_pid to fork container_init inside the configured cgroup & network namespace
        char go_signal = '1';
        if (write(parent_to_child_pipe[1], &go_signal, 1) != 1) {
            Utils::handle_error("Failed to write sync to child");
        }
        close(parent_to_child_pipe[1]);

        ContainerState cs{};
        cs.id = m_spec.id;
        cs.name = m_spec.name.empty() ? "quiver-" + m_spec.id.substr(0, 12) : m_spec.name;
        cs.image = m_spec.image;
        cs.pid = m_child_pid;
        cs.net_pid = m_network.get_net_pid();
        cs.status = "running";
        cs.hostname = m_spec.hostname.empty() ? m_spec.id.substr(0, 6) : m_spec.hostname;
        cs.rootfs_path = m_spec.rootfs_path;
        cs.bundle_path = m_store.container_bundle_dir(m_spec.id).string();
        cs.working_dir = m_spec.working_dir.empty() ? "/" : m_spec.working_dir;
        cs.env = m_spec.env;
        cs.command = m_spec.command.empty() ? std::vector<std::string>{"/bin/sh"} : m_spec.command;
        cs.use_vfs = m_spec.use_vfs;
        cs.no_remove = m_spec.no_remove;
        cs.volumes = m_spec.volumes;
        cs.ports = m_spec.ports;
        cs.resources = m_spec.resources;

        if (!m_store.save_container(cs)) {
            Monitor::get_instance().log("ERROR", "Container", "Unable to save container state for " + m_spec.id);
        }

        m_term.start_server(m_pty_args, m_spec.id, m_child_pid);

        int status = 0;
        waitpid(m_child_pid, &status, 0);

        m_store.update_container_status(m_spec.id, "exited");
        m_network.teardown();
        cgroup.cleanup();

        if (!m_spec.no_remove) {
            std::error_code ec;
            fs::remove_all(filesystem_dir, ec);
            if (ec) {
                Utils::remove_directory_recursively(filesystem_dir.string());
            }
        }

        Monitor::get_instance().log("INFO", "Container", "Container " + m_spec.id + " finished with exit code: " + std::to_string(WEXITSTATUS(status)));
        exit(EXIT_SUCCESS);
    }
}

void Container::run_container(int slave_fd, const fs::path& filesystem_dir) {
    if (setsid() == ERR) {
        Utils::handle_error("setsid error");
    }
    m_term.redirect_io(slave_fd);
    close(slave_fd);

    std::string hname = m_spec.hostname.empty() ? m_spec.id.substr(0, 6) : m_spec.hostname;
    if (sethostname(hname.c_str(), hname.size()) == ERR) {
        Utils::handle_error("Unable to set hostname");
    }

    fs::path final_filesystem;
    if (m_spec.use_vfs) {
        final_filesystem = filesystem_dir;
        Utils::ensure_dirs(final_filesystem);
        fs::copy(m_spec.rootfs_path, final_filesystem,
                 fs::copy_options::recursive | fs::copy_options::copy_symlinks | fs::copy_options::overwrite_existing);
    } else {
        fs::path upper = filesystem_dir / "upper";
        fs::path work = filesystem_dir / "work";
        fs::path merged = filesystem_dir / "merged";

        Utils::ensure_dirs(upper);
        Utils::ensure_dirs(work);
        Utils::ensure_dirs(merged);

        Mount::overlay(m_spec.rootfs_path, upper, work, merged);
        final_filesystem = merged;
    }

    if (mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) == ERR) {
        // Warning if non-fatal
    }

    if (mount(final_filesystem.c_str(), final_filesystem.c_str(), NULL, MS_BIND | MS_REC, NULL) == ERR) {
        Utils::handle_error("Unable to bind mount final_filesystem");
    }

    if (mount(NULL, final_filesystem.c_str(), NULL, MS_PRIVATE | MS_REC, NULL) == ERR) {
        Utils::handle_error("Unable to make final_filesystem private");
    }

    if (chdir(final_filesystem.c_str()) == ERR) {
        Utils::handle_error("Unable to change directory to " + final_filesystem.string());
    }

    Mount::volumes(final_filesystem.string(), m_spec.volumes);
    Utils::ensure_dirs("old_root");

    if (syscall(SYS_pivot_root, ".", "old_root") == ERR) {
        Utils::handle_error("Unable to pivot root: " + std::string(std::strerror(errno)));
    }

    if (chdir("/") == ERR) {
        Utils::handle_error("Unable to change dir to /");
    }

    Utils::ensure_dirs("/proc");
    Utils::ensure_dirs("/sys");
    Utils::ensure_dirs("/dev");
    Utils::ensure_dirs("/etc");

    Mount::proc("/proc", MS_NODEV | MS_NOEXEC | MS_NOSUID);
    Mount::sys("/sys", MS_NODEV | MS_NOSUID | MS_NOEXEC);
    Mount::tmpfs("/dev", 0, "mode=0755");

    DeviceManager::create_terminal_devices();

    if (GpuDeviceManager::is_gpu_requested(m_spec.resources)) {
        std::string gpu_err;
        if (!GpuDeviceManager::setup_device_mounts(m_spec.resources, "/old_root/dev", "/dev", gpu_err)) {
            Utils::handle_error("GPU device passthrough failed: " + gpu_err);
        }
        if (!GpuDeviceManager::inject_driver_libraries("/old_root", "/", gpu_err)) {
            std::cerr << "WARNING: GPU driver library injection incomplete: " << gpu_err << '\n';
        }
    }

    umount2("/old_root", MNT_DETACH);
    rmdir("/old_root");

    std::ofstream resolv("/etc/resolv.conf");
    if (resolv.is_open()) {
        resolv << "nameserver 10.0.2.3\n";
        resolv.close();
    }
    std::ofstream hosts("/etc/hosts");
    if (hosts.is_open()) {
        hosts << "127.0.0.1\tlocalhost\n";
        hosts << "::1\t\tlocalhost ip6-localhost ip6-loopback\n";
        hosts << "10.0.2.100\t" << hname << "\n";
        hosts.close();
    }

    // Working directory
    std::string work_dir = m_spec.working_dir.empty() ? "/" : m_spec.working_dir;
    Utils::ensure_dirs(work_dir);
    if (::chdir(work_dir.c_str()) == ERR) {
        Utils::handle_error("Unable to chdir to WorkingDir: " + work_dir);
    }

    // Environment variables
    ::clearenv();
    ::setenv("TERM", "xterm-256color", 1);
    ::setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    ::setenv("HOME", "/root", 1);
    for (const auto& env_kv : m_spec.env) {
        auto eq_pos = env_kv.find('=');
        if (eq_pos != std::string::npos) {
            std::string k = env_kv.substr(0, eq_pos);
            std::string v = env_kv.substr(eq_pos + 1);
            ::setenv(k.c_str(), v.c_str(), 1);
        }
    }

    if (GpuDeviceManager::is_gpu_requested(m_spec.resources)) {
        if (m_spec.resources.all_gpus) {
            ::setenv("NVIDIA_VISIBLE_DEVICES", "all", 1);
        } else {
            std::string visible;
            for (size_t i = 0; i < m_spec.resources.gpu_indices.size(); ++i) {
                if (i > 0) visible += ",";
                visible += std::to_string(m_spec.resources.gpu_indices[i]);
            }
            ::setenv("NVIDIA_VISIBLE_DEVICES", visible.c_str(), 1);
        }
        ::setenv("NVIDIA_DRIVER_CAPABILITIES", "compute,utility", 1);

        const char* current_ld = ::getenv("LD_LIBRARY_PATH");
        std::string new_ld = "/usr/lib:/usr/lib64:/usr/local/nvidia/lib:/usr/local/nvidia/lib64";
        if (current_ld && *current_ld) {
            new_ld = std::string(current_ld) + ":" + new_ld;
        }
        ::setenv("LD_LIBRARY_PATH", new_ld.c_str(), 1);
    }

    // Command resolution & execvp
    std::vector<std::string> effective_cmd;
    if (!m_spec.command.empty()) {
        effective_cmd = m_spec.command;
    } else {
        const char* shells[]{ "/bin/bash", "/bin/sh", "/bin/ash", nullptr };
        const char* shell = nullptr;
        for (int i = 0; shells[i] != nullptr; i++) {
            if (Utils::path_exists(shells[i])) {
                shell = shells[i];
                break;
            }
        }
        effective_cmd.push_back(shell ? shell : "/bin/sh");
    }

    std::vector<char*> c_argv;
    c_argv.reserve(effective_cmd.size() + 1);
    for (auto& arg : effective_cmd) {
        c_argv.push_back(arg.data());
    }
    c_argv.push_back(nullptr);

    ::execvp(c_argv[0], c_argv.data());
    Utils::handle_error(std::format("execvp('{}') failed: {}", c_argv[0], std::strerror(errno)));
}
