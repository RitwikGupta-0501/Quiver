#pragma once
#define ERR -1
#include <string>
#include <string_view>
#include <filesystem>
#include <cstdint>
#include <sys/types.h>

namespace fs = std::filesystem;

namespace Utils {
    enum class ArchiveCompression {
        Auto,
        Uncompressed, // application/vnd.oci.image.layer.v1.tar
        Gzip,         // application/vnd.oci.image.layer.v1.tar+gzip or docker .tar.gzip
        Zstd          // application/vnd.oci.image.layer.v1.tar+zstd
    };

    auto dir_exists(const fs::path& path) -> bool;
    auto path_exists(const fs::path& path) -> bool;
    auto ensure_dirs(const fs::path& path, mode_t mode = 0755) -> void;
    auto ensure_file(const fs::path& path) -> void;
    auto write_file(const fs::path& path, std::string_view buffer) -> void;
    auto append_file(const fs::path& path, std::string_view buffer) -> void;
    auto handle_error(const std::string&) -> void;
    auto print_usage() -> void;
    auto get_base_dir() -> std::string;
    auto get_sock_path(const pid_t& pid) -> std::string;
    auto get_sock_path(std::string_view container_id) -> std::string;
    auto get_filesystem_path(const pid_t& pid) -> std::string;
    auto get_vfs_path(const pid_t& pid) -> std::string;
    auto get_image_path(const std::string& image_name) -> std::string;
    auto get_logs_path(const pid_t& pid) -> std::string;
    auto get_logs_path(std::string_view container_id) -> std::string;
    auto generate_container_id() -> std::string;
    auto remove_directory_recursively(const std::string& path) -> int;
    auto extract_tarball(
        const fs::path& tarball_path,
        const fs::path& destination_path,
        ArchiveCompression compression = ArchiveCompression::Auto
    ) -> bool;
    auto verify_sha256_and_size(const fs::path& file_path, std::string_view expected_digest, std::uintmax_t expected_size) -> bool;
    auto get_host_oci_arch() -> std::string;
    auto parse_memory_string(std::string_view str) -> std::size_t;
    auto parse_cpus_string(std::string_view str) -> double;
    auto parse_pids_limit(std::string_view str) -> int;
}
