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

std::vector<VolumeMount> volumes{};
std::vector<std::string> commands{};
std::vector<std::pair<int,int>> forward_ports{};
std::string image_name{""};
std::string container_id{""};
std::string container_name{""};
pid_t container_pid{-1};
std::string root_fs{""};
std::string err{""};
bool vfs_f{false};
bool no_remove{false};

void CommandLineHandler::run(StateStore& store, const std::vector<std::string>& cmds){
    if (cmds.empty()) {
        Utils::print_usage();
        return;
    }
    size_t i{ 0 };
    while (i < cmds.size()) {
        const std::string& arg { cmds[i] };

        if (arg == "-v" || arg == "--volume") {
            if (i + 1 < cmds.size()) {
                std::string vol_str{ cmds[++i] };
                size_t pos{ vol_str.find(':') };
                if (pos != std::string::npos) {
                    std::string host_str{ vol_str.substr(0, pos) };
                    std::string cont_str{ vol_str.substr(pos + 1) };

                    if (host_str.empty() || cont_str.empty()) {
                        Utils::handle_error("Empty paths cannot be mounted");
                    }

                    if (host_str.front() != '/' || cont_str.front() != '/') {
                        Utils::handle_error("Paths must be absolute");
                    }
                    if (!Utils::path_exists(host_str)){
                        Utils::handle_error("Host path " + host_str + " doesn't exist");
                    }

                    VolumeMount vol{};
                    vol.host_path = host_str;
                    vol.container_path = cont_str;
                    volumes.emplace_back(vol);
                }
                else {
                    std::cerr << "Error: -v or --volume requires an argument\n";
                    return;
                }
            }
        }
        else if (arg == "-p" || arg == "--port") {
            if (i + 1 < cmds.size()) {
                std::string port_str{ cmds[++i] };
                size_t pos{ port_str.find(':') };
                if (pos != std::string::npos) {
                    try {
                        int host{ std::stoi(port_str.substr(0, pos)) };
                        int cont{ std::stoi(port_str.substr(pos + 1)) };
                        forward_ports.emplace_back(host, cont);
                    } catch (...) {
                        std::cerr << "Error: Invalid port format " << port_str << "\n";
                        return;
                    }
                }
            } else {
                std::cerr << "Error: -p or --port requires an argument\n";
                return;
            }
        }
        else if(arg == "-n" || arg == "--name"){
            if(i+1 < cmds.size()){
                container_name = cmds[++i];
            }
            else{
                std::cerr << "Error: -n or --name requires an argument\n";
                return;
            }
        }
        else if(arg == "--vfs"){
            vfs_f = true;
        }
        else if(arg == "--no-remove"){
            no_remove = true;
        }
        else if (arg == "-i" || arg == "--image") {
            if (i + 1 < cmds.size()) {
                image_name = cmds[++i];
            }
        }
        else {
            std::cerr << "Unknown argument: " << arg << "\n";
            Utils::print_usage();
            return;
        }
        i++;
    }

    if (image_name.empty()) {
        std::cerr << "Error: Image name required (-i)\n";
        return;
    }

    CommandLineHandler::pull(store, {image_name});
    container_id = Utils::generate_container_id();
    Container container{ container_name, container_id.substr(0,6), root_fs, volumes, forward_ports, container_id, store, image_name, vfs_f, no_remove };
    container.exec("/bin/bash", commands);
}

void CommandLineHandler::attach(StateStore& store, const std::vector<std::string>& cmds){
    if(cmds.size() != 1){
        Utils::print_usage();
        return;
    }
    container_id = cmds[0];
    auto opt = store.get_container(container_id);
    if (!opt) {
        std::cerr << "Error: Container " << container_id << " not found.\n";
        return;
    }
    container_pid = opt->pid;
    Container container;
    container.connect_to_server(container_pid);
}

void CommandLineHandler::ps(StateStore& store, const std::vector<std::string>& cmds){
    if(cmds.size() > 1){
        Utils::print_usage();
        return;
    }
    std::vector<ContainerState> containers{};
    if(cmds.empty()){
        containers = store.list_containers(true);
    }
    else if(cmds.size() == 1 && cmds[0] == "-a"){
        containers = store.list_containers(false);
    }
    else{
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

void CommandLineHandler::rm(StateStore& store, const std::vector<std::string>& cmds){
    if(cmds.empty()){
        Utils::print_usage();
        return;
    }
    for(const std::string& cmd : cmds){
        auto opt = store.get_container(cmd);
        if(!opt){
            std::cerr << "Error: Container " << cmd << " doesn't exist\n";
            continue;
        }

        if (opt->status == "stopped" || opt->status == "exited") {
            if (store.remove_container(opt->id)) {
                std::cout << "Container " << opt->id << " removed.\n";
            } else {
                std::cerr << "Error: Failed to remove container " << opt->id << '\n';
            }
        }
        else {
            std::cerr << "Error: Container " << opt->id << " is running.\n";
        }
    }
}

void CommandLineHandler::image(StateStore& store, ImageManager& img_manager, const std::vector<std::string>& cmds){
    if (cmds.empty()) {
        Utils::print_usage();
        return;
    }

    if(cmds[0] == "ls"){
        std::vector<ImageMetadata> images = store.list_images();

        std::cout << "NAME\tTAG\tCREATED AT\tROOTFS PATH\n";
        for (const auto& img : images) {
            std::cout << img.name << "\t"
                << img.tag << "\t"
                << img.created_at << "\t"
                << img.rootfs_path << "\n";
        }
    }
    else if(cmds[0] == "rm"){
        if (cmds.size() < 2) Utils::handle_error("Error: image_name:tag is required after rm");
        size_t no_cmds{ cmds.size() };
        for(size_t i{1}; i < no_cmds; ++i){
            image_name = cmds[i];
            size_t pos{ image_name.find(':') };
            std::string name{};
            std::string tag{};
            if(pos != std::string::npos){
                name = image_name.substr(0, pos);
                tag = image_name.substr(pos + 1);
            }
            else{
                name = image_name;
                tag = "latest";
            }
            std::string full_ref = name + ":" + tag;
            std::vector<ContainerState> containers = store.list_containers_by_image(full_ref);
            if(containers.empty()){
                containers = store.list_containers_by_image(name);
            }
            if(!containers.empty()){
                Utils::handle_error("Error: Image " + image_name + " is being used");
            }
            if(store.remove_image(name, tag)) {
                std::cout << image_name << " removed from state store\n";
            }
            else{
                std::cout << "Error: Cannot remove " << image_name << " from state store\n";
                exit(EXIT_FAILURE);
            }
            std::string local_err;
            if(img_manager.remove(image_name, local_err)) {
                std::cout << "Image " << image_name << " removed successfully from local storage.\n";
            } else {
                std::cout << "Failed to remove image " << image_name << " from local storage: " << local_err << '\n';
                exit(EXIT_FAILURE);
            }
        }

    }
    else if(cmds[0] == "cls"){
        if(cmds.size() == 2){
            image_name = cmds[1];
            std::vector<ContainerState> containers = store.list_containers_by_image(image_name);

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
        }
        else{
            Utils::print_usage();
        }
    }
    else{
        Utils::print_usage();
    }
}

void CommandLineHandler::volume(StateStore& store, const std::vector<std::string>& cmds){
    if(cmds.empty()){
        Utils::print_usage();
        return;
    }
    if(cmds[0] == "ls"){
        std::cout << "CONTAINER ID\tHOST PATH\tCONTAINER PATH\tREAD ONLY\n";
        for (const auto& c : store.list_containers(false)) {
            for (const auto& v : c.volumes) {
                std::cout << c.id.substr(0, 12) << "\t"
                    << v.host_path << "\t"
                    << v.container_path << "\t"
                    << (v.read_only ? "ro" : "rw") << "\n";
            }
        }
    }
    else if(cmds[0] == "rm"){
        if (cmds.size() < 2) {
            Utils::print_usage();
            return;
        }
        // Remove matching volume paths across containers
        for (size_t i = 1; i < cmds.size(); ++i) {
            std::string target = cmds[i];
            for (auto c : store.list_containers(false)) {
                auto prev_size = c.volumes.size();
                std::erase_if(c.volumes, [&](const VolumeMount& v) {
                    return v.host_path == target || v.container_path == target;
                });
                if (c.volumes.size() != prev_size) {
                    store.save_container(c);
                    std::cout << "Removed volume " << target << " from container " << c.id.substr(0, 12) << "\n";
                }
            }
        }
    }
    else{
        Utils::print_usage();
    }
}

void CommandLineHandler::network(StateStore& store, const std::vector<std::string>& cmds){
    if(cmds.empty()){
        Utils::print_usage();
        return;
    }
    if(cmds[0] == "ls"){
        std::cout << "CONTAINER ID\tHOST PORT\tCONTAINER PORT\tPROTOCOL\n";
        for (const auto& c : store.list_containers(false)) {
            for (const auto& p : c.ports) {
                std::cout << c.id.substr(0, 12) << "\t"
                    << p.host_port << "\t"
                    << p.container_port << "\t"
                    << p.protocol << "\n";
            }
        }
    }
    else if(cmds[0] == "rm"){
        if (cmds.size() < 2) {
            Utils::print_usage();
            return;
        }
        for (size_t i = 1; i < cmds.size(); ++i) {
            int port = std::stoi(cmds[i]);
            for (auto c : store.list_containers(false)) {
                auto prev_size = c.ports.size();
                std::erase_if(c.ports, [&](const PortMapping& p) {
                    return p.host_port == port;
                });
                if (c.ports.size() != prev_size) {
                    store.save_container(c);
                    std::cout << "Removed port " << port << " from container " << c.id.substr(0, 12) << "\n";
                }
            }
        }
    }
    else if(cmds[0] == "add"){
        if(cmds.size() < 3){
            Utils::print_usage();
            return;
        }
        container_id = cmds[1];
        auto opt = store.get_container(container_id);
        if(!opt){
            Utils::handle_error("Container " + container_id + " doesn't exist");
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

            if(cs.status == "running" && cs.pid > 0) {
                Network::add_port_forward(cs.pid, h_port, c_port);
            }
        }
        store.save_container(cs);
    }
    else{
        Utils::print_usage();
    }
}

void CommandLineHandler::create(StateStore& store, const std::vector<std::string>& cmds){
    if(cmds.empty()) {
        Utils::print_usage();
        return;
    }
    if(cmds[0] == "volume"){
        if(cmds.size() < 3){
            Utils::print_usage();
            return;
        }
        container_id = cmds[1];
        auto opt = store.get_container(container_id);
        if(!opt){
            Utils::handle_error("Container " + container_id + " doesn't exist");
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
        store.save_container(cs);
    }
    else {
        Utils::print_usage();
    }
}

void CommandLineHandler::pull(StateStore& store, const std::vector<std::string>& cmds){
    if(cmds.size() != 1){
        Utils::print_usage();
        return;
    }
    image_name = cmds[0];

    ImageManager img_manager(store);
    std::string out_path{};
    std::string pull_err{};
    if (!img_manager.pull(image_name, out_path, pull_err)) {
        std::cerr << "Error pulling image: " << pull_err << '\n';
        exit(EXIT_FAILURE);
    }
    root_fs = out_path;
}

void CommandLineHandler::start(StateStore& store, const std::vector<std::string>& cmds){
    if(cmds.size() != 1){
        std::cout << "Error: Container id is required\n";
        Utils::print_usage();
        return;
    }
    container_id = cmds[0];

    auto opt = store.get_container(container_id);
    if (!opt) {
        std::cerr << "Error: Container " << container_id << " not found.\n";
        return;
    }
    ContainerState cs = *opt;

    std::string c_name = cs.name;
    image_name = cs.image;

    std::vector<std::pair<int,int>> ports{};
    for (const auto& p : cs.ports) {
        ports.emplace_back(p.host_port, p.container_port);
    }

    Container container{
        c_name,
        cs.hostname,
        cs.rootfs_path,
        cs.volumes,
        ports,
        cs.id,
        store,
        image_name,
        cs.use_vfs,
        cs.no_remove
    };

    if (store.update_container_status(cs.id, "running")) {
        std::cout << "Starting container " << cs.id << "...\n";
    } else {
        std::cout << "Failed to update status of " << cs.id << " to RUNNING.\n";
        return;
    }

    container.exec("/bin/bash", commands);
}

void CommandLineHandler::stop(StateStore& store, const std::vector<std::string>& cmds){
    if(cmds.size() != 1){
        std::cout << "Error: Container id is required\n";
        Utils::print_usage();
        return;
    }
    container_id = cmds[0];

    auto opt = store.get_container(container_id);
    if(!opt) {
        std::cerr << "Error: Container not found.\n";
        return;
    }

    container_pid = opt->pid;
    pid_t net_pid{ opt->net_pid };

    Container container{};
    container.stop(container_pid, net_pid);

    if (store.update_container_status(opt->id, "stopped")) {
        std::cout << "Container " << opt->id << " stopped successfully.\n";
    } else {
        std::cout << "Failed to update status of " << opt->id << " in state store.\n";
    }
}

void CommandLineHandler::vfs(StateStore& store, const std::vector<std::string>& cmds){
    if(cmds.size() != 2){
        Utils::print_usage();
        return;
    }
    if(cmds[0] == "rm"){
        container_id = cmds[1];
        auto opt = store.get_container(container_id);
        if(opt){
            if(opt->no_remove){
                if(!opt->bundle_path.empty()){
                    if(Utils::remove_directory_recursively(opt->bundle_path) == ERR){
                        Utils::handle_error("Unable to remove vfs path");
                    }
                    store.remove_container(opt->id);
                }
                else{
                    Utils::handle_error(container_id + " has no vfs linked");
                }
            }
            else{
                Utils::handle_error("--no-remove flag was not specified while starting container");
            }
        }
        else{
            Utils::handle_error("Provided " + container_id + " doesn't exist");
        }
    }
    else{
        Utils::print_usage();
    }
}
