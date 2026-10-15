#include "utils.hpp"
#include "state_store.hpp"
#include "image_manager.hpp"
#include "command_line_handler.hpp"
#include "container.hpp"
#include "cgroup.hpp"
#include "gpu_manager.hpp"
#include <cstdlib>
#include <unistd.h>
#include <vector>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <thread>
#include <csignal>
#include <unordered_map>

CommandLineHandler::CommandLineHandler(StateStore& store, ImageManager& img_manager)
    : m_store(store), m_img_manager(img_manager) {}

bool CommandLineHandler::parse_container_options(
    const std::vector<std::string>& cmds,
    ContainerSpec& spec,
    std::string& error
) {
    size_t i = 0;
    while (i < cmds.size()) {
        const std::string& arg = cmds[i];

        if (arg == "-v" || arg == "--volume") {
            if (i + 1 < cmds.size()) {
                std::string vol_str = cmds[++i];
                size_t pos1 = vol_str.find(':');
                if (pos1 != std::string::npos) {
                    std::string host_str = vol_str.substr(0, pos1);
                    std::string cont_str;
                    bool read_only = false;
                    size_t pos2 = vol_str.find(':', pos1 + 1);
                    if (pos2 != std::string::npos) {
                        cont_str = vol_str.substr(pos1 + 1, pos2 - (pos1 + 1));
                        std::string mode = vol_str.substr(pos2 + 1);
                        if (mode == "ro") {
                            read_only = true;
                        } else if (mode != "rw") {
                            error = "Invalid volume mode '" + mode + "': must be 'ro' or 'rw'";
                            return false;
                        }
                    } else {
                        cont_str = vol_str.substr(pos1 + 1);
                    }

                    if (host_str.empty() || cont_str.empty()) {
                        error = "Empty paths cannot be mounted: " + vol_str;
                        return false;
                    }
                    if (host_str.front() != '/' || cont_str.front() != '/') {
                        error = "Paths must be absolute: " + vol_str;
                        return false;
                    }
                    if (!Utils::path_exists(host_str)) {
                        error = "Host path " + host_str + " doesn't exist";
                        return false;
                    }

                    VolumeMount vol{};
                    vol.host_path = host_str;
                    vol.container_path = cont_str;
                    vol.read_only = read_only;
                    spec.volumes.push_back(vol);
                } else {
                    error = "-v or --volume requires host:container[:ro|rw] format";
                    return false;
                }
            } else {
                error = "-v or --volume requires an argument";
                return false;
            }
        } else if (arg == "-p" || arg == "--port") {
            if (i + 1 < cmds.size()) {
                std::string port_str = cmds[++i];
                size_t pos = port_str.find(':');
                if (pos != std::string::npos) {
                    try {
                        int host = std::stoi(port_str.substr(0, pos));
                        int cont = std::stoi(port_str.substr(pos + 1));
                        if (host < 1 || host > 65535 || cont < 1 || cont > 65535) {
                            error = "Port numbers must be between 1 and 65535: " + port_str;
                            return false;
                        }
                        PortMapping pm{};
                        pm.host_port = host;
                        pm.container_port = cont;
                        pm.protocol = "tcp";
                        spec.ports.push_back(pm);
                    } catch (...) {
                        error = "Invalid port format " + port_str;
                        return false;
                    }
                } else {
                    error = "Invalid port format " + port_str + " (expected host:container)";
                    return false;
                }
            } else {
                error = "-p or --port requires an argument";
                return false;
            }
        } else if (arg == "-n" || arg == "--name") {
            if (i + 1 < cmds.size()) {
                spec.name = cmds[++i];
            } else {
                error = "-n or --name requires an argument";
                return false;
            }
        } else if (arg == "--vfs") {
            spec.use_vfs = true;
        } else if (arg == "--no-remove") {
            spec.no_remove = true;
        } else if (arg == "-m" || arg == "--memory") {
            if (i + 1 < cmds.size()) {
                try {
                    spec.resources.memory_max_bytes = Utils::parse_memory_string(cmds[++i]);
                } catch (const std::exception& e) {
                    error = "Invalid --memory value: " + std::string(e.what());
                    return false;
                }
            } else {
                error = arg + " requires an argument";
                return false;
            }
        } else if (arg == "--cpus") {
            if (i + 1 < cmds.size()) {
                try {
                    spec.resources.cpus = Utils::parse_cpus_string(cmds[++i]);
                } catch (const std::exception& e) {
                    error = "Invalid --cpus value: " + std::string(e.what());
                    return false;
                }
            } else {
                error = "--cpus requires an argument";
                return false;
            }
        } else if (arg == "--pids-limit") {
            if (i + 1 < cmds.size()) {
                try {
                    spec.resources.pids_max = Utils::parse_pids_limit(cmds[++i]);
                } catch (const std::exception& e) {
                    error = "Invalid --pids-limit value: " + std::string(e.what());
                    return false;
                }
            } else {
                error = "--pids-limit requires an argument";
                return false;
            }
        } else if (arg == "--gpus") {
            if (i + 1 < cmds.size()) {
                try {
                    auto req = Utils::parse_gpus_string(cmds[++i]);
                    spec.resources.all_gpus = req.all_gpus;
                    spec.resources.gpu_indices = std::move(req.gpu_indices);
                } catch (const std::exception& e) {
                    error = "Invalid --gpus value: " + std::string(e.what());
                    return false;
                }
            } else {
                error = "--gpus requires an argument (e.g., all or 0,1)";
                return false;
            }
        } else if (arg == "-i" || arg == "--image") {
            if (i + 1 < cmds.size()) {
                spec.image = cmds[++i];
            } else {
                error = "-i or --image requires an argument";
                return false;
            }
        } else if (arg == "--") {
            for (size_t j = i + 1; j < cmds.size(); ++j) {
                spec.command.push_back(cmds[j]);
            }
            break;
        } else if (!arg.starts_with('-')) {
            for (size_t j = i; j < cmds.size(); ++j) {
                spec.command.push_back(cmds[j]);
            }
            break;
        } else {
            error = "Unknown argument: " + arg;
            return false;
        }
        i++;
    }

    if (spec.image.empty()) {
        error = "Image name required (-i)";
        return false;
    }

    std::string out_path;
    if (!m_img_manager.pull(spec.image, out_path, error)) {
        return false;
    }

    std::string img_name, img_tag;
    size_t colon_pos = spec.image.find(':');
    if (colon_pos != std::string::npos) {
        img_name = spec.image.substr(0, colon_pos);
        img_tag = spec.image.substr(colon_pos + 1);
    } else {
        img_name = spec.image;
        img_tag = "latest";
    }

    auto img_meta = m_store.get_image(img_name, img_tag);
    if (img_meta.has_value()) {
        spec.rootfs_path = img_meta->rootfs_path;
        spec.env = img_meta->default_env;
        if (!img_meta->working_dir.empty()) {
            spec.working_dir = img_meta->working_dir;
        }
        if (spec.command.empty()) {
            if (!img_meta->default_entrypoint.empty() || !img_meta->default_cmd.empty()) {
                for (const auto& ep : img_meta->default_entrypoint) {
                    spec.command.push_back(ep);
                }
                for (const auto& c : img_meta->default_cmd) {
                    spec.command.push_back(c);
                }
            }
        }
    } else {
        spec.rootfs_path = out_path;
    }

    if (spec.hostname.empty()) {
        spec.hostname = spec.id.substr(0, 6);
    }
    if (spec.name.empty()) {
        spec.name = "quiver-" + spec.id.substr(0, 12);
    }

    return true;
}

void CommandLineHandler::run(const std::vector<std::string>& cmds) {
    if (cmds.empty()) {
        Utils::print_usage();
        return;
    }

    ContainerSpec spec;
    spec.id = Utils::generate_container_id();

    std::string error;
    if (!parse_container_options(cmds, spec, error)) {
        std::cerr << "Error: " << error << '\n';
        Utils::print_usage();
        return;
    }

    Container container{ spec, m_store };
    container.start();
}

void CommandLineHandler::attach(const std::vector<std::string>& cmds) {
    if (cmds.size() != 1) {
        Utils::print_usage();
        return;
    }
    std::string cid = cmds[0];
    auto opt = m_store.get_container(cid);
    if (!opt) {
        std::cerr << "Error: Container " << cid << " not found.\n";
        return;
    }
    Container::attach(opt->pid);
}

void CommandLineHandler::ps(const std::vector<std::string>& cmds) {
    if (cmds.size() > 1) {
        Utils::print_usage();
        return;
    }
    std::vector<ContainerState> containers;
    if (cmds.empty()) {
        containers = m_store.list_containers(true);
    } else if (cmds.size() == 1 && cmds[0] == "-a") {
        containers = m_store.list_containers(false);
    } else {
        Utils::print_usage();
        return;
    }

    std::cout << "CONTAINER ID\tNAME\tIMAGE\tSTATUS\tCREATED AT\tBUNDLE PATH\n";
    for (const auto& container : containers) {
        std::cout << container.id.substr(0, 12) << "\t"
            << container.name << "\t"
            << container.image << "\t"
            << container.status << "\t"
            << container.created_at << "\t"
            << container.bundle_path << '\n';
    }
}

void CommandLineHandler::rm(const std::vector<std::string>& cmds) {
    if (cmds.empty()) {
        Utils::print_usage();
        return;
    }
    for (const std::string& cmd : cmds) {
        auto opt = m_store.get_container(cmd);
        if (!opt) {
            std::cerr << "Error: Container " << cmd << " doesn't exist\n";
            continue;
        }

        if (opt->status == "stopped" || opt->status == "exited") {
            if (m_store.remove_container(opt->id)) {
                std::cout << "Container " << opt->id << " removed.\n";
            } else {
                std::cerr << "Error: Failed to remove container " << opt->id << '\n';
            }
        } else {
            std::cerr << "Error: Container " << opt->id << " is running.\n";
        }
    }
}

void CommandLineHandler::image(const std::vector<std::string>& cmds) {
    if (cmds.empty()) {
        Utils::print_usage();
        return;
    }

    if (cmds[0] == "ls") {
        std::vector<ImageMetadata> images = m_store.list_images();

        std::cout << "NAME\tTAG\tCREATED AT\tROOTFS PATH\n";
        for (const auto& img : images) {
            std::cout << img.name << "\t"
                << img.tag << "\t"
                << img.created_at << "\t"
                << img.rootfs_path << "\n";
        }
    } else if (cmds[0] == "rm") {
        if (cmds.size() < 2) Utils::handle_error("Error: image_name:tag is required after rm");
        for (size_t i = 1; i < cmds.size(); ++i) {
            std::string img_ref = cmds[i];
            size_t pos = img_ref.find(':');
            std::string name;
            std::string tag;
            if (pos != std::string::npos) {
                name = img_ref.substr(0, pos);
                tag = img_ref.substr(pos + 1);
            } else {
                name = img_ref;
                tag = "latest";
            }
            std::string full_ref = name + ":" + tag;
            std::vector<ContainerState> containers = m_store.list_containers_by_image(full_ref);
            if (containers.empty()) {
                containers = m_store.list_containers_by_image(name);
            }
            if (!containers.empty()) {
                Utils::handle_error("Error: Image " + img_ref + " is being used");
            }
            if (m_store.remove_image(name, tag)) {
                std::cout << img_ref << " removed from state store\n";
            } else {
                std::cout << "Error: Cannot remove " << img_ref << " from state store\n";
                exit(EXIT_FAILURE);
            }
            std::string local_err;
            if (m_img_manager.remove(img_ref, local_err)) {
                std::cout << "Image " << img_ref << " removed successfully from local storage.\n";
            } else {
                std::cout << "Failed to remove image " << img_ref << " from local storage: " << local_err << '\n';
                exit(EXIT_FAILURE);
            }
        }
    } else if (cmds[0] == "cls") {
        if (cmds.size() == 2) {
            std::string img_name = cmds[1];
            std::vector<ContainerState> containers = m_store.list_containers_by_image(img_name);

            std::cout << "CONTAINER ID\tNAME\tIMAGE\tPID\tSTATUS\tCREATED AT\tHOSTNAME\n";
            for (const auto& container : containers) {
                std::cout << container.id.substr(0, 12) << "\t"
                    << container.name << "\t"
                    << container.image << "\t"
                    << container.pid << "\t"
                    << container.status << "\t"
                    << container.created_at << "\t"
                    << container.hostname << "\n";
            }
        } else {
            Utils::print_usage();
        }
    } else {
        Utils::print_usage();
    }
}

void CommandLineHandler::volume(const std::vector<std::string>& cmds) {
    if (cmds.empty()) {
        Utils::print_usage();
        return;
    }
    if (cmds[0] == "ls") {
        std::cout << "CONTAINER ID\tHOST PATH\tCONTAINER PATH\tREAD ONLY\n";
        for (const auto& c : m_store.list_containers(false)) {
            for (const auto& v : c.volumes) {
                std::cout << c.id.substr(0, 12) << "\t"
                    << v.host_path << "\t"
                    << v.container_path << "\t"
                    << (v.read_only ? "ro" : "rw") << "\n";
            }
        }
    } else if (cmds[0] == "rm") {
        if (cmds.size() < 2) {
            Utils::print_usage();
            return;
        }
        for (size_t i = 1; i < cmds.size(); ++i) {
            std::string target = cmds[i];
            for (auto c : m_store.list_containers(false)) {
                auto prev_size = c.volumes.size();
                std::erase_if(c.volumes, [&](const VolumeMount& v) {
                    return v.host_path == target || v.container_path == target;
                });
                if (c.volumes.size() != prev_size) {
                    m_store.save_container(c);
                    std::cout << "Removed volume " << target << " from container " << c.id.substr(0, 12) << "\n";
                }
            }
        }
    } else {
        Utils::print_usage();
    }
}

void CommandLineHandler::network(const std::vector<std::string>& cmds) {
    if (cmds.empty()) {
        Utils::print_usage();
        return;
    }
    if (cmds[0] == "ls") {
        std::cout << "CONTAINER ID\tHOST PORT\tCONTAINER PORT\tPROTOCOL\n";
        for (const auto& c : m_store.list_containers(false)) {
            for (const auto& p : c.ports) {
                std::cout << c.id.substr(0, 12) << "\t"
                    << p.host_port << "\t"
                    << p.container_port << "\t"
                    << p.protocol << "\n";
            }
        }
    } else if (cmds[0] == "rm") {
        if (cmds.size() < 2) {
            Utils::print_usage();
            return;
        }
        for (size_t i = 1; i < cmds.size(); ++i) {
            int port = 0;
            try {
                port = std::stoi(cmds[i]);
            } catch (const std::exception&) {
                std::cerr << "Error: Invalid port number: " << cmds[i] << '\n';
                continue;
            }
            if (port < 1 || port > 65535) {
                std::cerr << "Error: Port number must be between 1 and 65535: " << cmds[i] << '\n';
                continue;
            }
            for (auto c : m_store.list_containers(false)) {
                auto prev_size = c.ports.size();
                std::erase_if(c.ports, [&](const PortMapping& p) {
                    return p.host_port == port;
                });
                if (c.ports.size() != prev_size) {
                    if (c.status == "running" && c.pid > 0) {
                        Network::remove_port_forward(c.pid, port);
                    }
                    m_store.save_container(c);
                    std::cout << "Removed port " << port << " from container " << c.id.substr(0, 12) << "\n";
                }
            }
        }
    } else if (cmds[0] == "add") {
        if (cmds.size() < 3) {
            Utils::print_usage();
            return;
        }
        std::string cid = cmds[1];
        auto opt = m_store.get_container(cid);
        if (!opt) {
            Utils::handle_error("Container " + cid + " doesn't exist");
            return;
        }
        ContainerState cs = *opt;
        for (size_t i = 2; i < cmds.size(); ++i) {
            std::string net_str = cmds[i];
            size_t sep_pos = net_str.find(':');
            if (sep_pos == std::string::npos) {
                std::cerr << "Invalid port format: " << net_str << " (expected host:container)\n";
                continue;
            }
            int h_port = 0;
            int c_port = 0;
            try {
                h_port = std::stoi(net_str.substr(0, sep_pos));
                c_port = std::stoi(net_str.substr(sep_pos + 1));
            } catch (const std::exception&) {
                std::cerr << "Error: Invalid port format: " << net_str << '\n';
                continue;
            }
            if (h_port < 1 || h_port > 65535 || c_port < 1 || c_port > 65535) {
                std::cerr << "Error: Port numbers must be between 1 and 65535: " << net_str << '\n';
                continue;
            }
            PortMapping pm{};
            pm.host_port = h_port;
            pm.container_port = c_port;
            pm.protocol = "tcp";
            cs.ports.push_back(pm);

            if (cs.status == "running" && cs.pid > 0) {
                Network::add_port_forward(cs.pid, h_port, c_port);
            }
        }
        m_store.save_container(cs);
    } else {
        Utils::print_usage();
    }
}

void CommandLineHandler::create(const std::vector<std::string>& cmds) {
    if (cmds.empty()) {
        Utils::print_usage();
        return;
    }
    if (cmds[0] == "volume") {
        if (cmds.size() < 3) {
            Utils::print_usage();
            return;
        }
        std::string cid = cmds[1];
        auto opt = m_store.get_container(cid);
        if (!opt) {
            Utils::handle_error("Container " + cid + " doesn't exist");
            return;
        }
        ContainerState cs = *opt;
        for (size_t i = 2; i < cmds.size(); ++i) {
            std::string vol_str = cmds[i];
            size_t pos1 = vol_str.find(':');
            if (pos1 == std::string::npos) {
                std::cerr << "Invalid volume format: " << vol_str << "\n";
                continue;
            }
            std::string host_str = vol_str.substr(0, pos1);
            std::string cont_str;
            bool read_only = false;
            size_t pos2 = vol_str.find(':', pos1 + 1);
            if (pos2 != std::string::npos) {
                cont_str = vol_str.substr(pos1 + 1, pos2 - (pos1 + 1));
                std::string mode = vol_str.substr(pos2 + 1);
                if (mode == "ro") {
                    read_only = true;
                } else if (mode != "rw") {
                    std::cerr << "Invalid volume mode: " << mode << "\n";
                    continue;
                }
            } else {
                cont_str = vol_str.substr(pos1 + 1);
            }
            VolumeMount vm{host_str, cont_str, read_only};
            cs.volumes.push_back(vm);
        }
        m_store.save_container(cs);
    } else if (cmds[0] == "gpu") {
        if (cmds.size() < 3) {
            std::cerr << "Usage: quiver create gpu <container_id> <all|0,1,...>\n";
            return;
        }
        std::string cid = cmds[1];
        auto opt = m_store.get_container(cid);
        if (!opt) {
            Utils::handle_error("Container " + cid + " doesn't exist");
            return;
        }
        ContainerState cs = *opt;
        try {
            auto req = Utils::parse_gpus_string(cmds[2]);
            cs.resources.all_gpus = req.all_gpus;
            cs.resources.gpu_indices = std::move(req.gpu_indices);
        } catch (const std::exception& e) {
            std::cerr << "Error: Invalid --gpus value: " << e.what() << '\n';
            return;
        }
        if (m_store.save_container(cs)) {
            std::cout << "Configured GPU access for container " << cid << '\n';
        } else {
            std::cerr << "Error: Failed to update container state for " << cid << '\n';
        }
    } else {
        ContainerSpec spec{};
        spec.id = Utils::generate_container_id();

        std::string error;
        if (!parse_container_options(cmds, spec, error)) {
            std::cerr << "Error: " << error << '\n';
            Utils::print_usage();
            return;
        }

        ContainerState cs{};
        cs.id = spec.id;
        cs.name = spec.name;
        cs.image = spec.image;
        cs.pid = -1;
        cs.net_pid = -1;
        cs.status = "created";
        cs.hostname = spec.hostname;
        cs.rootfs_path = spec.rootfs_path;
        cs.bundle_path = m_store.container_bundle_dir(spec.id).string();
        cs.working_dir = spec.working_dir.empty() ? "/" : spec.working_dir;
        cs.env = spec.env;
        cs.command = spec.command.empty() ? std::vector<std::string>{"/bin/sh"} : spec.command;
        cs.use_vfs = spec.use_vfs;
        cs.no_remove = spec.no_remove;
        cs.volumes = spec.volumes;
        cs.ports = spec.ports;
        cs.resources = spec.resources;

        if (m_store.save_container(cs)) {
            std::cout << cs.id << '\n';
        } else {
            std::cerr << "Error: Failed to save created container state\n";
        }
    }
}

void CommandLineHandler::pull(const std::vector<std::string>& cmds) {
    if (cmds.size() != 1) {
        Utils::print_usage();
        return;
    }
    std::string img_name = cmds[0];
    std::string out_path;
    std::string pull_err;
    if (!m_img_manager.pull(img_name, out_path, pull_err)) {
        std::cerr << "Error pulling image: " << pull_err << '\n';
        exit(EXIT_FAILURE);
    }
}

void CommandLineHandler::start(const std::vector<std::string>& cmds) {
    if (cmds.empty()) {
        std::cout << "Error: Container id is required\n";
        Utils::print_usage();
        return;
    }
    for (const auto& cid : cmds) {
        auto opt = m_store.get_container(cid);
        if (!opt) {
            std::cerr << "Error: Container " << cid << " not found.\n";
            continue;
        }
        ContainerSpec spec{};
        spec.id = opt->id;
        spec.name = opt->name;
        spec.image = opt->image;
        spec.hostname = opt->hostname;
        spec.rootfs_path = opt->rootfs_path;
        spec.command = opt->command;
        spec.use_vfs = opt->use_vfs;
        spec.no_remove = opt->no_remove;
        spec.volumes = opt->volumes;
        spec.ports = opt->ports;
        spec.resources = opt->resources;
        spec.working_dir = opt->working_dir;
        spec.env = opt->env;

        if (spec.env.empty()) {
            std::string img_name, img_tag;
            size_t colon_pos = spec.image.find(':');
            if (colon_pos != std::string::npos) {
                img_name = spec.image.substr(0, colon_pos);
                img_tag = spec.image.substr(colon_pos + 1);
            } else {
                img_name = spec.image;
                img_tag = "latest";
            }
            auto img_meta = m_store.get_image(img_name, img_tag);
            if (img_meta.has_value()) {
                spec.env = img_meta->default_env;
                if (!img_meta->working_dir.empty() && (spec.working_dir.empty() || spec.working_dir == "/")) {
                    spec.working_dir = img_meta->working_dir;
                }
            }
        }

        std::cout << "Starting container " << spec.id << "...\n";
        Container container{ spec, m_store };
        container.start();
    }
}

void CommandLineHandler::stop(const std::vector<std::string>& cmds) {
    if (cmds.empty()) {
        std::cout << "Error: Container id is required\n";
        Utils::print_usage();
        return;
    }
    for (const auto& cid : cmds) {
        auto opt = m_store.get_container(cid);
        if (!opt) {
            std::cerr << "Error: Container " << cid << " not found.\n";
            continue;
        }
        Container::stop(opt->pid, opt->net_pid);
        if (m_store.update_container_status(opt->id, "stopped")) {
            std::cout << "Container " << opt->id << " stopped successfully.\n";
        } else {
            std::cout << "Failed to update status of " << opt->id << " in state store.\n";
        }
    }
}

void CommandLineHandler::vfs(const std::vector<std::string>& cmds) {
    if (cmds.size() != 2) {
        Utils::print_usage();
        return;
    }
    if (cmds[0] == "rm") {
        std::string cid = cmds[1];
        auto opt = m_store.get_container(cid);
        if (opt) {
            if (opt->no_remove) {
                if (!opt->bundle_path.empty()) {
                    if (Utils::remove_directory_recursively(opt->bundle_path) == ERR) {
                        Utils::handle_error("Unable to remove vfs path");
                    }
                    m_store.remove_container(opt->id);
                } else {
                    Utils::handle_error(cid + " has no vfs linked");
                }
            } else {
                Utils::handle_error("--no-remove flag was not specified while starting container");
            }
        } else {
            Utils::handle_error("Provided " + cid + " doesn't exist");
        }
    } else {
        Utils::print_usage();
    }
}

namespace {
volatile sig_atomic_t g_stats_running = 1;
void stats_sigint_handler(int) {
    g_stats_running = 0;
}
} // namespace

void CommandLineHandler::stats(const std::vector<std::string>& cmds) {
    bool no_stream = false;
    std::vector<std::string> requested_ids;

    for (const auto& cmd : cmds) {
        if (cmd == "--no-stream" || cmd == "--once") {
            no_stream = true;
        } else if (!cmd.starts_with('-')) {
            requested_ids.push_back(cmd);
        }
    }

    if (!isatty(STDOUT_FILENO)) {
        no_stream = true;
    }

    struct sigaction sa{};
    struct sigaction old_sa{};
    if (!no_stream) {
        g_stats_running = 1;
        sa.sa_handler = stats_sigint_handler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, &old_sa);
    }

    NvmlTelemetryCollector nvml_collector;

    struct ContainerCpuSample {
        std::uint64_t usage_usec{0};
        std::chrono::steady_clock::time_point time;
    };
    std::unordered_map<std::string, ContainerCpuSample> prev_samples;

    auto render_once = [&](bool print_output) {
        auto running_containers = m_store.list_containers(true);
        std::vector<ContainerState> targets;
        if (requested_ids.empty()) {
            targets = std::move(running_containers);
        } else {
            for (const auto& req : requested_ids) {
                for (const auto& cs : running_containers) {
                    if (cs.id.starts_with(req) || cs.name == req) {
                        targets.push_back(cs);
                        break;
                    }
                }
            }
        }

        if (targets.empty()) {
            if (print_output) {
                if (!no_stream) {
                    std::cout << "\033[2J\033[H";
                }
                std::cout << "CONTAINER ID   NAME             CPU %     MEM USAGE / LIMIT       PIDS        GPU UTIL   GPU VRAM\n";
            }
            return;
        }

        auto now = std::chrono::steady_clock::now();
        std::vector<std::pair<ContainerState, CgroupMetrics>> metrics_list;
        std::vector<double> cpu_pcts;

        for (const auto& cs : targets) {
            auto cg_dir = CgroupManager::find_container_cgroup_dir(cs.id, cs.pid);
            CgroupMetrics cm{};
            if (cg_dir) {
                (void)CgroupManager::read_metrics(*cg_dir, cm);
            }

            double cpu_pct = 0.0;
            auto it = prev_samples.find(cs.id);
            if (it != prev_samples.end()) {
                auto delta_time_us = std::chrono::duration_cast<std::chrono::microseconds>(now - it->second.time).count();
                if (delta_time_us > 0 && cm.cpu_usage_usec >= it->second.usage_usec) {
                    auto delta_usage_us = cm.cpu_usage_usec - it->second.usage_usec;
                    cpu_pct = (static_cast<double>(delta_usage_us) / static_cast<double>(delta_time_us)) * 100.0;
                }
            }
            prev_samples[cs.id] = ContainerCpuSample{ cm.cpu_usage_usec, now };

            metrics_list.emplace_back(cs, cm);
            cpu_pcts.push_back(cpu_pct);
        }

        if (print_output) {
            if (!no_stream) {
                std::cout << "\033[2J\033[H";
            }
            std::cout << "CONTAINER ID   NAME             CPU %     MEM USAGE / LIMIT       PIDS        GPU UTIL   GPU VRAM\n";

            for (size_t idx = 0; idx < metrics_list.size(); ++idx) {
                const auto& cs = metrics_list[idx].first;
                const auto& cm = metrics_list[idx].second;
                double cpu_pct = cpu_pcts[idx];

                std::vector<GpuDeviceTelemetry> gpu_telemetry;
                bool gpu_req = GpuDeviceManager::is_gpu_requested(cs.resources);
                if (gpu_req && nvml_collector.is_available()) {
                    gpu_telemetry = nvml_collector.collect_metrics(cs.resources, cm.pids);
                }

                std::cout << GpuDeviceManager::format_stats_row(
                    cs.id, cs.name, cpu_pct, cm, gpu_telemetry, gpu_req
                ) << '\n';
            }
            std::cout.flush();
        }
    };

    if (no_stream) {
        render_once(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        render_once(true);
    } else {
        render_once(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        while (g_stats_running) {
            render_once(true);
            for (int i = 0; i < 10 && g_stats_running; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        sigaction(SIGINT, &old_sa, nullptr);
    }
}


