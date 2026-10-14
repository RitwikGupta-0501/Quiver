#include "utils.hpp"
#include "state_store.hpp"
#include "image_manager.hpp"
#include "command_line_handler.hpp"
#include "container.hpp"
#include <cstdlib>
#include <unistd.h>
#include <vector>
#include <iostream>
#include <algorithm>

CommandLineHandler::CommandLineHandler(StateStore& store, ImageManager& img_manager)
    : m_store(store), m_img_manager(img_manager) {}

void CommandLineHandler::run(const std::vector<std::string>& cmds) {
    if (cmds.empty()) {
        Utils::print_usage();
        return;
    }

    ContainerSpec spec;
    spec.id = Utils::generate_container_id();

    size_t i = 0;
    while (i < cmds.size()) {
        const std::string& arg = cmds[i];

        if (arg == "-v" || arg == "--volume") {
            if (i + 1 < cmds.size()) {
                std::string vol_str = cmds[++i];
                size_t pos = vol_str.find(':');
                if (pos != std::string::npos) {
                    std::string host_str = vol_str.substr(0, pos);
                    std::string cont_str = vol_str.substr(pos + 1);

                    if (host_str.empty() || cont_str.empty()) {
                        Utils::handle_error("Empty paths cannot be mounted");
                    }
                    if (host_str.front() != '/' || cont_str.front() != '/') {
                        Utils::handle_error("Paths must be absolute");
                    }
                    if (!Utils::path_exists(host_str)) {
                        Utils::handle_error("Host path " + host_str + " doesn't exist");
                    }

                    VolumeMount vol{};
                    vol.host_path = host_str;
                    vol.container_path = cont_str;
                    spec.volumes.push_back(vol);
                } else {
                    std::cerr << "Error: -v or --volume requires an argument\n";
                    return;
                }
            }
        } else if (arg == "-p" || arg == "--port") {
            if (i + 1 < cmds.size()) {
                std::string port_str = cmds[++i];
                size_t pos = port_str.find(':');
                if (pos != std::string::npos) {
                    try {
                        int host = std::stoi(port_str.substr(0, pos));
                        int cont = std::stoi(port_str.substr(pos + 1));
                        PortMapping pm{};
                        pm.host_port = host;
                        pm.container_port = cont;
                        pm.protocol = "tcp";
                        spec.ports.push_back(pm);
                    } catch (...) {
                        std::cerr << "Error: Invalid port format " << port_str << "\n";
                        return;
                    }
                }
            } else {
                std::cerr << "Error: -p or --port requires an argument\n";
                return;
            }
        } else if (arg == "-n" || arg == "--name") {
            if (i + 1 < cmds.size()) {
                spec.name = cmds[++i];
            } else {
                std::cerr << "Error: -n or --name requires an argument\n";
                return;
            }
        } else if (arg == "--vfs") {
            spec.use_vfs = true;
        } else if (arg == "--no-remove") {
            spec.no_remove = true;
        } else if (arg == "-i" || arg == "--image") {
            if (i + 1 < cmds.size()) {
                spec.image = cmds[++i];
            } else {
                std::cerr << "Error: -i or --image requires an argument\n";
                return;
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
            std::cerr << "Unknown argument: " << arg << "\n";
            Utils::print_usage();
            return;
        }
        i++;
    }

    if (spec.image.empty()) {
        std::cerr << "Error: Image name required (-i)\n";
        return;
    }

    std::string out_path;
    std::string error;
    if (!m_img_manager.pull(spec.image, out_path, error)) {
        std::cerr << "Error pulling image: " << error << '\n';
        return;
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
            int port = std::stoi(cmds[i]);
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
                std::cerr << "Invalid port format: " << net_str << "\n";
                continue;
            }
            int h_port = std::stoi(net_str.substr(0, sep_pos));
            int c_port = std::stoi(net_str.substr(sep_pos + 1));
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
            size_t sep_pos = vol_str.find(':');
            if (sep_pos == std::string::npos) {
                std::cerr << "Invalid volume format: " << vol_str << "\n";
                continue;
            }
            VolumeMount vm{};
            vm.host_path = vol_str.substr(0, sep_pos);
            vm.container_path = vol_str.substr(sep_pos + 1);
            cs.volumes.push_back(vm);
        }
        m_store.save_container(cs);
    } else {
        Utils::print_usage();
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
            if (!img_meta->working_dir.empty()) {
                spec.working_dir = img_meta->working_dir;
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
