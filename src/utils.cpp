#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <cstring>
#include <iostream>
#include <unistd.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <sys/utsname.h>
#include <random>
#include <chrono>
#include <dirent.h>
#include <sys/wait.h>
#include <format>
#include <vector>
#include <cctype>
#include <cmath>
#include <atomic>
#include <thread>
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
    if (!parent_path.empty() && !dir_exists(parent_path)) ensure_dirs(parent_path);

    int fd = ::open(path.c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0644);
    if (fd < 0) [[unlikely]] {
        throw std::runtime_error(std::format("File Error: couldn't open '{}': {}", path.string(), std::strerror(errno)));
    }

    const char* ptr = buffer.data();
    size_t remaining = buffer.size();
    while (remaining > 0) {
        ssize_t written = ::write(fd, ptr, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            throw std::runtime_error(std::format("File Error: failed to write data to '{}': {}", path.string(), std::strerror(errno)));
        }
        ptr += written;
        remaining -= written;
    }
    ::close(fd);
}

auto Utils::append_file(const fs::path& path, std::string_view buffer) -> void {
    fs::path parent_path{path.parent_path()};
    if (!parent_path.empty() && !dir_exists(parent_path)) ensure_dirs(parent_path);

    std::ofstream file{path, std::ios::out | std::ios::app};
    if (!file.is_open()) [[unlikely]] {
        throw std::runtime_error(std::format("File Error: couldn't open '{}' for append", path.string()));
    }

    file << buffer;
    if (!file) [[unlikely]] {
        throw std::runtime_error(std::format("File Error: failed to append data to '{}'", path.string()));
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
    return get_sock_path(std::to_string(static_cast<long long>(pid)));
}

std::string Utils::get_sock_path(std::string_view container_id){
    std::string path{ get_base_dir() + "/containers/" + std::string(container_id) };
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
    return get_logs_path(std::to_string(static_cast<long long>(pid)));
}

std::string Utils::get_logs_path(std::string_view container_id){
    std::string path{ get_base_dir() + "/logs" };
    ensure_dirs(path);
    return path + "/" + std::string(container_id) + ".log";
}

void Utils::print_usage(){
std::cout << "Usage: quiver <command> [options] [arguments]\n\n"
              << "Commands:\n"
              << "  run [options] -i <image> [cmd]   Create and start a new container\n"
              << "      -i, --image <name>           Image to use (required)\n"
              << "      -n, --name <name>            Assign a name to the container\n"
              << "      -p, --port <host:cont>       Publish a container's port(s) to the host\n"
              << "      -v, --volume <host:cont>     Bind mount a volume\n"
              << "      -m, --memory <limit>         Memory limit (e.g., 512m, 2g)\n"
              << "      --cpus <quota>               Number of CPUs (e.g., 0.5, 2.0)\n"
              << "      --pids-limit <max>           Tune container pids limit (default: 512)\n"
              << "      --gpus <all|0,1,...>         Expose host GPUs to container\n"
              << "      --vfs                        Use VFS (copy) instead of OverlayFS for package manager related\n"
              << "      --no-remove                  Do not remove the filesystem after exit\n\n"

              << "  start <container_id> ...         Start one or more stopped containers\n"
              << "  stop <container_id> ...          Stop one or more running containers\n"
              << "  rm <container_id> ...            Remove one or more containers\n"
              << "  attach <container_id>            Attach local standard input, output, and error to a running container\n\n"

              << "  ps [-a]                          List containers\n"
              << "      -a                           Show all containers (default shows just running)\n\n"

              << "  stats [--no-stream] [cid...]     Display live container resource & GPU usage statistics\n\n"

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
              << "  create [options] -i <image> [cmd] Create a container without starting it\n"
              << "      --gpus <all|0,1,...>         Expose host GPUs to container\n"
              << "  create gpu <container_id> <all|0,1,...> Configure GPU access for a container\n"
              << "  create volume <container_id> <host:cont> ... Create a volume link for container\n\n"
              << "  vfs rm <container_id>            remove vfs for a container\n\n"
              << "  help                             Show this help message\n";
}

std::string Utils::generate_container_id() {
    static std::atomic<std::uint64_t> counter{0};
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    pid_t pid = ::getpid();
    auto tid_hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
    auto seq = counter.fetch_add(1, std::memory_order_relaxed);
    std::random_device rd;
    auto r1 = rd();
    auto r2 = rd();

    std::string input = std::to_string(now) + ":" + std::to_string(pid) + ":" +
                        std::to_string(tid_hash) + ":" + std::to_string(seq) + ":" +
                        std::to_string(r1) + ":" + std::to_string(r2);

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx) {
        if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
            EVP_DigestUpdate(ctx, input.data(), input.size()) == 1) {
            EVP_DigestFinal_ex(ctx, hash, &hash_len);
        }
        EVP_MD_CTX_free(ctx);
    }

    std::string hexString;
    hexString.reserve(hash_len * 2);
    static const char hexDigits[] = "0123456789abcdef";

    for (unsigned int i = 0; i < hash_len; i++) {
        hexString += hexDigits[(hash[i] >> 4) & 0xF];
        hexString += hexDigits[hash[i] & 0xF];
    }

    return hexString;
}

int Utils::remove_directory_recursively(const std::string& path) {
    ::chmod(path.c_str(), 0755);
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
            remove_directory_recursively(full_path);
        } else {
            ::chmod(full_path.c_str(), 0644);
            unlink(full_path.c_str());
        }
    }
    closedir(dir);

    return rmdir(path.c_str());
}

bool Utils::extract_tarball(
    const fs::path& tarball_path,
    const fs::path& destination_path,
    ArchiveCompression compression
) {
    ensure_dirs(destination_path);
    pid_t pid = fork();

    if (pid == ERR) {
        perror("fork failed");
        return false;
    }
    if (pid == 0) {
        std::vector<const char*> args;
        args.push_back("tar");
        args.push_back("--no-same-owner");
        args.push_back("--no-same-permissions");
        args.push_back("-x");

        switch (compression) {
            case ArchiveCompression::Gzip:
                args.push_back("-z");
                break;
            case ArchiveCompression::Zstd:
                args.push_back("--zstd");
                break;
            case ArchiveCompression::Uncompressed:
                break;
            case ArchiveCompression::Auto:
            default:
                args.push_back("-a");
                break;
        }

        std::string tb_str = tarball_path.string();
        std::string dest_str = destination_path.string();
        args.push_back("-f");
        args.push_back(tb_str.c_str());
        args.push_back("-C");
        args.push_back(dest_str.c_str());
        args.push_back(nullptr);

        execvp("tar", const_cast<char* const*>(args.data()));
        perror("execvp tar failed");
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) == ERR) {
        perror("waitpid failed");
        return false;
    }

    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        if (exit_code == 0) {
            return true;
        }
        std::cerr << "Tar exited with error code: " << exit_code << std::endl;
        return false;
    }
    return false;
}

auto Utils::verify_sha256_and_size(
    const fs::path& file_path,
    std::string_view expected_digest,
    std::uintmax_t expected_size
) -> bool {
    std::error_code ec;
    auto size = fs::file_size(file_path, ec);
    if (ec || size != expected_size) {
        return false;
    }

    std::string_view target_digest = expected_digest;
    if (target_digest.starts_with("sha256:")) {
        target_digest.remove_prefix(7);
    }

    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return false;
    }

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return false;
    }

    constexpr size_t buffer_size = 64 * 1024; // 64 KB
    std::vector<char> buffer(buffer_size);

    while (file.read(buffer.data(), buffer_size) || file.gcount() > 0) {
        if (EVP_DigestUpdate(ctx, buffer.data(), file.gcount()) != 1) {
            EVP_MD_CTX_free(ctx);
            return false;
        }
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    if (EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return false;
    }
    EVP_MD_CTX_free(ctx);

    std::string hex_str;
    hex_str.reserve(hash_len * 2);
    static constexpr char hex_chars[] = "0123456789abcdef";
    for (unsigned int i = 0; i < hash_len; ++i) {
        hex_str.push_back(hex_chars[(hash[i] >> 4) & 0x0F]);
        hex_str.push_back(hex_chars[hash[i] & 0x0F]);
    }

    if (hex_str.size() != target_digest.size()) {
        return false;
    }
    for (size_t i = 0; i < hex_str.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(hex_str[i])) !=
            std::tolower(static_cast<unsigned char>(target_digest[i]))) {
            return false;
        }
    }
    return true;
}

auto Utils::get_host_oci_arch() -> std::string {
    struct utsname uts{};
    if (uname(&uts) != 0) {
        return "amd64";
    }
    std::string_view machine = uts.machine;
    if (machine == "x86_64") {
        return "amd64";
    }
    if (machine == "aarch64" || machine == "arm64") {
        return "arm64";
    }
    if (machine == "armv7l") {
        return "arm";
    }
    if (machine == "riscv64") {
        return "riscv64";
    }
    return uts.machine;
}

auto Utils::parse_memory_string(std::string_view str) -> std::size_t {
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front()))) {
        str.remove_prefix(1);
    }
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) {
        str.remove_suffix(1);
    }
    if (str.empty() || str.front() == '-') {
        throw std::invalid_argument("Invalid memory limit string");
    }

    std::string s(str);
    size_t processed = 0;
    double val = 0.0;
    try {
        val = std::stod(s, &processed);
    } catch (...) {
        throw std::invalid_argument("Failed to parse number in memory string");
    }

    if (processed == 0 || val <= 0.0 || std::isnan(val) || std::isinf(val)) {
        throw std::invalid_argument("Memory limit must be greater than zero");
    }

    std::string_view suffix = str.substr(processed);
    while (!suffix.empty() && std::isspace(static_cast<unsigned char>(suffix.front()))) {
        suffix.remove_prefix(1);
    }

    std::string lower_suffix;
    lower_suffix.reserve(suffix.size());
    for (char c : suffix) {
        lower_suffix.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    std::uint64_t multiplier = 1;
    if (lower_suffix.empty() || lower_suffix == "b") {
        multiplier = 1ULL;
    } else if (lower_suffix == "k" || lower_suffix == "kb" || lower_suffix == "kib") {
        multiplier = 1024ULL;
    } else if (lower_suffix == "m" || lower_suffix == "mb" || lower_suffix == "mib") {
        multiplier = 1024ULL * 1024ULL;
    } else if (lower_suffix == "g" || lower_suffix == "gb" || lower_suffix == "gib") {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
    } else {
        throw std::invalid_argument("Unknown memory unit suffix: " + lower_suffix);
    }

    double total = val * static_cast<double>(multiplier);
    if (total <= 0.0 || std::isinf(total)) {
        throw std::invalid_argument("Resulting memory value invalid");
    }

    auto bytes = static_cast<std::size_t>(std::llround(total));
    if (bytes == 0) {
        throw std::invalid_argument("Memory limit must be greater than 0");
    }
    return bytes;
}

auto Utils::parse_cpus_string(std::string_view str) -> double {
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front()))) {
        str.remove_prefix(1);
    }
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) {
        str.remove_suffix(1);
    }
    if (str.empty() || str.front() == '-') {
        throw std::invalid_argument("Invalid cpus string");
    }

    std::string s(str);
    size_t processed = 0;
    double cpus = 0.0;
    try {
        cpus = std::stod(s, &processed);
    } catch (...) {
        throw std::invalid_argument("Failed to parse number in cpus string");
    }

    if (processed != s.size() || cpus <= 0.0 || std::isnan(cpus) || std::isinf(cpus)) {
        throw std::invalid_argument("CPUs value must be a positive number");
    }
    return cpus;
}

auto Utils::parse_pids_limit(std::string_view str) -> int {
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front()))) {
        str.remove_prefix(1);
    }
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) {
        str.remove_suffix(1);
    }
    if (str.empty()) {
        throw std::invalid_argument("Empty pids limit string");
    }

    if (str == "max" || str == "-1") {
        return 0; // 0 or -1 indicates unlimited / max
    }

    if (str.front() == '-') {
        throw std::invalid_argument("PIDs limit must be positive or 'max'");
    }

    std::string s(str);
    size_t processed = 0;
    int pids = 0;
    try {
        pids = std::stoi(s, &processed);
    } catch (...) {
        throw std::invalid_argument("Failed to parse integer in pids limit string");
    }

    if (processed != s.size() || pids < 1) {
        throw std::invalid_argument("PIDs limit must be an integer >= 1");
    }
    return pids;
}

auto Utils::parse_gpus_string(std::string_view str) -> GpuRequest {
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front()))) {
        str.remove_prefix(1);
    }
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) {
        str.remove_suffix(1);
    }
    if (str.empty()) {
        throw std::invalid_argument("Empty --gpus specification");
    }

    std::string lower;
    lower.reserve(str.size());
    for (char c : str) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (lower == "all") {
        return GpuRequest{.all_gpus = true, .gpu_indices = {}};
    }

    std::vector<int> indices;
    size_t start = 0;
    while (start <= str.size()) {
        size_t comma = str.find(',', start);
        std::string_view token = (comma == std::string_view::npos)
            ? str.substr(start)
            : str.substr(start, comma - start);

        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) {
            token.remove_prefix(1);
        }
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
            token.remove_suffix(1);
        }

        if (token.empty()) {
            throw std::invalid_argument("Empty token in --gpus specification");
        }

        for (char c : token) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                throw std::invalid_argument("Invalid GPU index '" + std::string(token) + "': must be non-negative integer");
            }
        }

        try {
            int idx = std::stoi(std::string(token));
            indices.push_back(idx);
        } catch (...) {
            throw std::invalid_argument("Failed to parse GPU index: " + std::string(token));
        }

        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }

    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

    return GpuRequest{.all_gpus = false, .gpu_indices = std::move(indices)};
}

