#include "../include/container.hpp"
#include "../include/utils.hpp"
#include "../include/network.hpp"
#include "../include/mount.hpp"
#include "../include/device_manager.hpp"
#include <cstdlib>
#include <iostream>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/sysmacros.h>
#include <cstring>
#include <net/if.h>

bool Container::m_vfs{ false };
bool Container::m_no_remove{ false };
pid_t Container::m_child_pid{ -1 };
StateStore* Container::m_store{ nullptr };
std::string Container::m_container_name{ "" };
std::string Container::m_container_id{ "" };
std::string Container::m_new_hostname{ "" };
std::string Container::m_new_fs{ "" };
std::vector<VolumeMount> Container::m_volumes{};
std::vector<std::string> Container::m_commands{};
std::vector<std::pair<int,int>> Container::m_forward_ports{};
Terminal Container::m_term{};
Terminal::PtyArgs Container::m_pty_args{};
std::string Container::m_image_name{ "" };

Container::Container(const std::string& container_name,
                     const std::string& hostname,
                     const std::string& new_fs,
                     const std::vector<VolumeMount>& volumes,
                     const std::vector<std::pair<int,int>>& ports,
                     const std::string& container_id,
                     StateStore& store,
                     const std::string& image_name,
                     bool vfs,
                     bool no_remove){
    m_new_hostname = hostname;
    m_new_fs = new_fs;
    m_volumes = volumes;
    m_forward_ports = ports;
    m_image_name = image_name;
    m_vfs = vfs;
    m_no_remove = no_remove;
    m_store = &store;
    m_container_id = container_id;
    m_container_name = container_name;
}

void Container::exec(const std::string& program_path, const std::vector<std::string>& commands) {
    m_commands = commands;
    run(program_path, m_container_id);
}

void Container::secure_kill(const pid_t& pid) {
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

void Container::stop(const pid_t& container_pid, const pid_t& net_pid) {
    secure_kill(container_pid);
    if (net_pid > 0) {
        secure_kill(net_pid);
    }
    std::string api_socket = "/tmp/slirp4netns-" + std::to_string(container_pid) + ".sock";
    if (unlink(api_socket.c_str()) == 0) {
        std::cout << "Cleaned up network socket: " << api_socket << '\n';
    }
}

void Container::set_filesystem(const std::string& path){
    m_new_fs = path;
}

void Container::connect_to_server(const pid_t& container_pid){
    m_term.connect_to_server(container_pid);
}

void Container::connect_to_other_container(const pid_t& target_pid, int host_port, int target_port){
    Network::connect_namespaces(target_pid, host_port, target_port);
}

void Container::manage_container(const std::string& path, const std::string& filesystem_dir) {
    if(unshare(CLONE_NEWUSER) == ERR){
        Utils::handle_error("Unable to clone new usernamespace");
    }
    ioctl(STDIN_FILENO, TIOCGWINSZ, &m_pty_args.window_size);
    m_term.start_pty_session(m_pty_args);
    if (fork() != 0) {
        exit(0);
    }
    if (setsid() == -1) {
        Utils::handle_error("setsid failed for manager");
    }

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
        close(parent_to_child_pipe[1]);
        close(child_to_parent_pipe[0]);

        if (unshare(CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWNS) != 0) {
            Utils::handle_error("unshare failed");
        }

        char ready_signal{ '1' };
        if (write(child_to_parent_pipe[1], &ready_signal, 1) != 1) {
            Utils::handle_error("could not signal parent");
        }
        close(child_to_parent_pipe[1]);

        char sync_signal{};
        if (read(parent_to_child_pipe[0], &sync_signal, 1) != 1) {
            Utils::handle_error("Failed to receive sync from parent");
        }
        close(parent_to_child_pipe[0]);

        pid_t container_init { fork() };
        if (container_init == ERR) {
            Utils::handle_error("fork for PID namespace failed");
        }

        if (container_init == 0) {
            ContainerArgs args{ m_new_hostname, m_new_fs, path, m_pty_args.slave_fd, filesystem_dir, m_commands };
            run_container(args);
        } else {
            close(m_pty_args.slave_fd);

            int status{};
            waitpid(container_init, &status, 0);
            exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
        }

    } else {
        close(parent_to_child_pipe[0]);
        close(child_to_parent_pipe[1]);

        char ready_signal{};
        if (read(child_to_parent_pipe[0], &ready_signal, 1) != 1) {
            Utils::handle_error("could not read signal from child");
        }
        close(child_to_parent_pipe[0]);

        setup_user_namespace();

        char go_signal{ '1' };
        if (write(parent_to_child_pipe[1], &go_signal, 1) != 1) {
            Utils::handle_error("Failed to write sync to child");
        }
        close(parent_to_child_pipe[1]);

        if(m_forward_ports.empty()){
            if(Network::setup_networking(m_child_pid) != 0){
                Utils::handle_error("Unable to setup networking");
            }
        }
        else{
            if(Network::setup_networking_with_ports(m_child_pid, m_forward_ports) != 0){
                Utils::handle_error("Unable to setup networking");
            }
        }

        StateStore store{ Utils::get_base_dir() };

        if(store.container_exists(m_container_id)){
            if(!store.update_container_status(m_container_id, "running", m_child_pid, Network::get_net_pid())){
                std::cerr << "Warning: Could not update status to running\n";
            }
        }
        else{
            ContainerState cs{};
            cs.id = m_container_id;
            cs.name = m_container_name.empty() ? "quiver-" + m_container_id.substr(0, 12) : m_container_name;
            cs.image = m_image_name;
            cs.pid = m_child_pid;
            cs.net_pid = Network::get_net_pid();
            cs.status = "running";
            cs.hostname = m_container_id.substr(0, 6);
            cs.rootfs_path = m_new_fs;
            cs.bundle_path = store.container_bundle_dir(m_container_id).string();
            cs.command = m_commands.empty() ? std::vector<std::string>{"/bin/sh"} : m_commands;
            cs.use_vfs = m_vfs;
            cs.no_remove = m_no_remove;
            cs.volumes = m_volumes;
            for(const auto& p : m_forward_ports){
                PortMapping pm{};
                pm.host_port = p.first;
                pm.container_port = p.second;
                pm.protocol = "tcp";
                cs.ports.push_back(pm);
            }
            if(!store.save_container(cs)){
                Utils::handle_error("Unable to save container state");
            }
        }

        m_term.start_server(m_pty_args, m_container_id ,m_child_pid);

        int status{};
        waitpid(m_child_pid, &status, 0);

        if (!store.update_container_status(m_container_id, "exited")) {
            Utils::handle_error("Unable to update container status to EXITED");
        }
        if(!m_no_remove && store.container_exists(m_container_id)){
            if(Utils::remove_directory_recursively(filesystem_dir) == ERR){
                Utils::handle_error("Unable to remove overlayfs dir");
            }
        }
        secure_kill(Network::get_net_pid());
        exit(EXIT_SUCCESS);
    }
}

void Container::run(const std::string& path, const std::string& container_id) {

    std::string filesystem_dir{};
    pid_t temp_pid{ getpid() };
    if(!m_store->container_exists(container_id)){
        filesystem_dir =  m_vfs ? Utils::get_vfs_path(temp_pid) : Utils::get_filesystem_path(temp_pid);
    }
    else{
        auto cont = m_store->get_container(container_id);
        filesystem_dir = m_vfs ? (m_no_remove && cont ? cont->bundle_path : Utils::get_vfs_path(temp_pid)) : Utils::get_filesystem_path(temp_pid);
    }
    if(!m_vfs){
        std::string upper { filesystem_dir + "/upper" };
        std::string merged { filesystem_dir + "/merged" };
        std::string work  { filesystem_dir + "/work" };

        Utils::ensure_dirs(upper);
        Utils::ensure_dirs(work);
        Utils::ensure_dirs(merged);
    }

    pid_t manager_pid{ fork() };
    if (manager_pid == ERR) {
        Utils::handle_error("fork for manager failed");
    }

    if (manager_pid == 0) {
        manage_container(path, filesystem_dir);
    } else {
        close(m_pty_args.master_fd);
        close(m_pty_args.slave_fd);

        std::cerr << "Container started." << '\n';
        std::cerr << "To attach, run: quiver attach " << m_container_id << '\n';

        int status{};
        waitpid(manager_pid, &status, 0);
    }
}

void Container::run_container(const ContainerArgs& args) {
    if (setsid() == ERR) {
        Utils::handle_error("setsid error");
    }
    m_term.redirect_io(args.slave_fd);
    close(args.slave_fd);

    if (sethostname(args.hostname.c_str(), args.hostname.size()) == ERR)
        Utils::handle_error("Unable to set hostname");

    std::string filesystem_path{ args.filesystem_dir };
    std::string final_filesystem{};
    if(m_vfs){
        final_filesystem = filesystem_path;
        Utils::ensure_dirs(final_filesystem);
        std::string copy_command{ "cp -a '" + m_new_fs + "/.' '" + final_filesystem + "'" };
        if(system(copy_command.c_str()) != 0){
            Utils::handle_error("Error: Cannot copy root filesystem");
        }
    }
    else{
        final_filesystem = filesystem_path + "/merged";
    }

    std::cerr << "DEBUG: Container init PID: " << getpid() << '\n';

    if (mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) == ERR) {
        std::cerr << "WARNING: Could not make / private: " << strerror(errno) << '\n';
    }

    std::cerr << "DEBUG: Mounting overlayfs..." << '\n';
    if(!m_vfs){
        std::string upper { filesystem_path + "/upper" };
        std::string work { filesystem_path + "/work" };
        std::string overlay_options { "lowerdir=" + args.rootfs_path +
                                      ",upperdir=" + upper +
                                      ",workdir=" + work };

        if (mount("overlay", final_filesystem.c_str(), "overlay", MS_NODEV, overlay_options.c_str()) == ERR) {
            std::cerr << "ERROR: Failed to mount overlayfs: " << strerror(errno) << '\n';
            std::cerr << "DEBUG: Options were: " << overlay_options << '\n';
            Utils::handle_error("Cannot mount overlay filesystem");
        }

        std::cerr << "DEBUG: Overlay mounted successfully at: " << final_filesystem << '\n';
        std::cerr << "DEBUG: Setting up merged as mount point..." << '\n';
    }

    if (mount(final_filesystem.c_str(), final_filesystem.c_str(), NULL, MS_BIND | MS_REC, NULL) == ERR) {
        Utils::handle_error("Unable to bind mount final_filesystem");
    }

    if (mount(NULL, final_filesystem.c_str(), NULL, MS_PRIVATE | MS_REC, NULL) == ERR) {
        Utils::handle_error("Unable to make final_filesystem private");
    }

    if (chdir(final_filesystem.c_str()) == ERR) {
        Utils::handle_error("Unable to change directory to " + final_filesystem);
    }

    Mount::volumes(final_filesystem, m_volumes);
    std::cerr << "DEBUG: Creating old_root..." << '\n';
    Utils::ensure_dirs("old_root");
    std::cerr << "DEBUG: Performing pivot_root..." << '\n';

    if (syscall(SYS_pivot_root, ".", "old_root") == ERR) {
        std::cerr << "ERROR: pivot_root failed, errno=" << errno
                  << " (" << strerror(errno) << ")" << '\n';
        Utils::handle_error("Unable to pivot root");
    }

    std::cerr << "DEBUG: pivot_root successful!" << '\n';
    if (chdir("/") == ERR)
        Utils::handle_error("Unable to change dir to /");

    std::cerr << "DEBUG: Creating mount point directories..." << '\n';
    std::string proc { "/proc" };
    std::string sys { "/sys" };
    std::string dev { "/dev" };
    std::string etc { "/etc" };

    Utils::ensure_dirs(proc);
    Utils::ensure_dirs(sys);
    Utils::ensure_dirs(dev);
    Utils::ensure_dirs(etc);
    std::cerr << "DEBUG: Mounting special filesystems..." << '\n';

    Mount::proc(proc, MS_NODEV | MS_NOEXEC | MS_NOSUID);
    Mount::sys(sys, MS_NODEV | MS_NOSUID | MS_NOEXEC);
    Mount::tmpfs(dev, 0, "mode=0755");

    std::cerr << "DEBUG: Setting up /dev devices from old_root..." << '\n';
    DeviceManager::create_terminal_devices();

    std::cerr << "DEBUG: Unmounting old_root..." << '\n';
    umount2("/old_root", MNT_DETACH);
    rmdir("/old_root");

    std::cout << "Container setup successful!" << '\n';

    std::ofstream resolv("/etc/resolv.conf");
    if (resolv.is_open()) {
        resolv << "nameserver 10.0.2.3\n";
        resolv.close();
    }
    std::ofstream hosts("/etc/hosts");
    if (hosts.is_open()) {
        hosts << "127.0.0.1\tlocalhost\n";
        hosts << "::1\t\tlocalhost ip6-localhost ip6-loopback\n";
        hosts << "10.0.2.100\t" << args.hostname << "\n";

        hosts.close();
    }

    clearenv();
    setenv("TERM", "xterm", 0);
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);

    const char* shell{ nullptr };
    const char* shells[]{ "/bin/bash", "/bin/sh", "/bin/ash", nullptr };

    for (int i = 0; shells[i] != nullptr; i++) {
        if(Utils::path_exists(shells[i])){
            shell = shells[i];
            break;
        }
    }
    execl(shell, shell, (char*)0);
    std::cerr << "ERROR: execl failed, errno=" << errno << " (" << strerror(errno) << ")" << '\n';
    Utils::handle_error("Failed to execute " + args.program_path);
}

void Container::setup_user_namespace() {
    std::string uid_map{ "0 1000 1\n" };
    std::string gid_map{ "0 1000 1\n" };

    std::string uid_map_path{ "/proc/self/uid_map" };
    std::string gid_map_path{ "/proc/self/gid_map" };
    std::string setgroups_path{ "/proc/self/setgroups" };

    Utils::write_file(setgroups_path, "deny\n");
    Utils::write_file(gid_map_path, gid_map);
    Utils::write_file(uid_map_path, uid_map);
}
