#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <string.h>
#include <iostream>
#include <unistd.h>
#include <openssl/sha.h>
#include <random>
#include <chrono>
#include <dirent.h>
#include <sys/wait.h>
#include <format>
#include "utils.hpp"

auto Utils::dir_exists(const fs::path& path) -> bool {
        return fs::is_directory(path);
}

auto Utils::path_exists(const fs::path& path) -> bool {
        return fs::exists(path);
}

auto Utils::ensure_dirs(const fs::path& path, mode_t mode) -> void {
        if(!dir_exists(path)) {
                fs::create_directories(path);
                fs::permissions(path, static_cast<fs::perms>(mode), fs::perm_options::replace);
        }
        if(!dir_exists(path)) [[unlikely]] {
                throw std::runtime_error(std::format("Directory Error: couldn't create '{}'", path.string()));
        }
}

auto Utils::ensure_file(const fs::path& path) -> void {
        if(!path_exists(path)) {
                fs::path parent_path{path.parent_path()};
                if(!parent_path.empty() && !dir_exists(parent_path)) ensure_dirs(parent_path);
                std::ofstream file{path};
                if(!file) [[unlikely]] {
                        throw std::runtime_error(std::format("File Error: failed to create '{}'", path.string()));
                }
        }
}

auto Utils::write_file(const fs::path& path, std::string_view buffer) -> void {
        fs::path parent_path{path.parent_path()};
        if(!parent_path.empty() && !dir_exists(parent_path)) ensure_dirs(parent_path);
        std::ofstream file{path, std::ios::app};

        if (!file.is_open()) [[unlikely]] {
                throw std::runtime_error(std::format("File Error: couldn't open '{}'", path.string()));
        }

        file << buffer;
        if (!file) [[unlikely]] {
                throw std::runtime_error(std::format("File Error: failed to write data to '{}'", path.string()));
        }
}

auto Utils::handle_error(const std::string& err) -> void {
        std::cerr << err << '\n';
        exit(EXIT_FAILURE);
}

std::string Utils::get_base_dir(){
    const char* home{ getenv("HOME") };
    std::string base{ home ? std::string(home) : "/tmp" };
    return base + "/.quiver";
}

std::string Utils::get_sock_path(const pid_t& pid){
    std::string path{ get_base_dir() + "/containers/" + std::to_string(static_cast<long long>(pid)) };
    ensure_dirs(path);
    return path + "/attach.sock";
}

std::string Utils::get_filesystem_path(const pid_t& pid){
    std::string path{ get_base_dir() + "/filesystems/" + std::to_string(static_cast<long long>(pid)) };
    ensure_dirs(path);
    return path;
}

std::string Utils::get_vfs_path(const pid_t &pid){
    std::string path{ get_base_dir() + "/vfs/" + std::to_string(static_cast<long long>(pid)) };
    return path;
}

std::string Utils::get_image_path(const std::string& image_name){
    std::string path{ get_base_dir() + "/images/" + image_name};
    return path;
}

std::string Utils::get_logs_path(const pid_t& pid){
    std::string path{ get_base_dir() + "/logs/" + std::to_string(static_cast<long long>(pid)) };
    ensure_dirs(path);
    return path;
}

void Utils::print_usage(){
std::cout << "Usage: quiver <command> [options] [arguments]\n\n"
              << "Commands:\n"
              << "  run [options] -i <image> [cmd]   Create and start a new container\n"
              << "      -i, --image <name>           Image to use (required)\n"
              << "      -n, --name <name>            Assign a name to the container\n"
              << "      -p, --port <host:cont>       Publish a container's port(s) to the host\n"
              << "      -v, --volume <host:cont>     Bind mount a volume\n"
              << "      --vfs                        Use VFS (copy) instead of OverlayFS for package manager related\n"
              << "      --no-remove                  Do not remove the filesystem after exit\n\n"

              << "  start <container_id> ...         Start one or more stopped containers\n"
              << "  stop <container_id> ...          Stop one or more running containers\n"
              << "  rm <container_id> ...            Remove one or more containers\n"
              << "  attach <container_id>            Attach local standard input, output, and error to a running container\n\n"

              << "  ps [-a]                          List containers\n"
              << "      -a                           Show all containers (default shows just running)\n\n"

              << "  pull <image_name>                Pull an image from a registry\n\n"
              << "  image <subcommand>               Manage images\n"
              << "      ls                           List available images\n"
              << "      rm <image:tag>               Remove an image\n"
              << "      cls <image_name>             List containers using a specific image\n\n"

              << "  volume <subcommand>              Manage volumes\n"
              << "      ls                           List all volumes\n"
              << "      rm <volume_id> ...           remove one or more volume links\n\n"

              << "  network <subcommand>             Manage networks\n"
              << "      ls                           List all network port mappings\n"
              << "      rm <network_id> ...          remove one or more network links\n"
              << "      add <container_id> [args]                                    \n"
              << "              <host:cont> ...      add a new network link\n\n"
              << "  create <subcommand>              Create resources\n"
              << "      volume <container_id> [args]                 \n"
              << "                  <host:cont> ...  Create a volume link for container\n\n"
              << "  vfs rm <container_id>            remove vfs for a container\n\n"
              << "  help                             Show this help message\n";
}

std::string Utils::generate_container_id() {
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::random_device rd;
    std::string input = std::to_string(now) + ":" + std::to_string(rd());

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.c_str()), input.size(), hash);

    std::string hexString;
    hexString.reserve(SHA256_DIGEST_LENGTH * 2);
    static const char hexDigits[] = "0123456789abcdef";

    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        hexString += hexDigits[(hash[i] >> 4) & 0xF];
        hexString += hexDigits[hash[i] & 0xF];
    }

    return hexString;
}

int Utils::remove_directory_recursively(const std::string& path) {
    DIR* dir{ opendir(path.c_str()) };
    if (!dir) {
        return ERR;
    }
    dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        std::string full_path{ path + "/" + entry->d_name };
        struct stat statbuf;
        if (lstat(full_path.c_str(), &statbuf) == ERR) {
            continue;
        }
        if (S_ISDIR(statbuf.st_mode)) {
            if (remove_directory_recursively(full_path) == ERR) {
            }
        } else {
            unlink(full_path.c_str());
        }
    }
    closedir(dir);

    return rmdir(path.c_str());
}

bool Utils::extract_tarball(const std::string& tarball_path, const std::string& destination_path) {
    pid_t pid{ fork() };

    if (pid == ERR) {
        perror("fork failed");
        return false;
    }
    else if (pid == 0) {
        execlp("tar", "tar", "-xzf", tarball_path.c_str(), "-C", destination_path.c_str(), NULL);
        perror("execlp failed");
        exit(ERR);
    }
    else {
        int status{};
        waitpid(pid, &status, 0);

        if (WIFEXITED(status)) {
            int exit_code = WEXITSTATUS(status);
            if (exit_code == 0) {
                return true;
            } else {
                std::cerr << "Tar exited with error code: " << exit_code << std::endl;
                return false;
            }
        }
    }
    return false;
}
