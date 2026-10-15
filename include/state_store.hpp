#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <sys/types.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

struct VolumeMount {
    std::string host_path{};
    std::string container_path{};
    bool read_only{false};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(VolumeMount, host_path, container_path, read_only)
};

struct PortMapping {
    int host_port{};
    int container_port{};
    std::string protocol{"tcp"};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(PortMapping, host_port, container_port, protocol)
};

struct ResourceLimits {
    std::size_t memory_max_bytes{0}; // 0 = unlimited
    double cpus{0.0};                // e.g., 2.0 cores; 0.0 = unlimited
    int pids_max{512};               // Default fork-bomb protection
    std::vector<int> gpu_indices{};  // e.g., {0, 1} for /dev/nvidia0, /dev/nvidia1
    bool all_gpus{false};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ResourceLimits, memory_max_bytes, cpus, pids_max, gpu_indices, all_gpus)
};

struct ContainerState {
    std::string id{};
    std::string name{};
    std::string image{};
    pid_t pid{-1};
    pid_t net_pid{-1};
    std::string status{"created"};   // "created", "running", "stopped", "exited"
    std::string created_at{};
    std::string hostname{};
    std::string rootfs_path{};       // Lowerdir image path
    std::string bundle_path{};       // ~/.quiver/containers/<id>
    std::string working_dir{"/"};
    std::vector<std::string> env{};
    std::vector<std::string> command{"/bin/sh"};
    bool use_vfs{false};
    bool no_remove{false};
    std::vector<VolumeMount> volumes{};
    std::vector<PortMapping> ports{};
    ResourceLimits resources{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        ContainerState, id, name, image, pid, net_pid, status, created_at,
        hostname, rootfs_path, bundle_path, working_dir, env, command,
        use_vfs, no_remove, volumes, ports, resources
    )
};

struct ImageMetadata {
    std::string name{};
    std::string tag{"latest"};
    std::string rootfs_path{};
    std::string created_at{};
    std::vector<std::string> default_env{};
    std::vector<std::string> default_entrypoint{};
    std::vector<std::string> default_cmd{"/bin/sh"};
    std::string working_dir{"/"};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        ImageMetadata, name, tag, rootfs_path, created_at,
        default_env, default_entrypoint, default_cmd, working_dir
    )
};

class StateStore {
public:
    explicit StateStore(fs::path base_dir);
    ~StateStore() = default;

    // Initialization
    void ensure_store_layout() const;

    // Container Lifecycle State (Lock-Free Per-Container Directory)
    bool save_container(const ContainerState& state) const;
    std::optional<ContainerState> get_container(std::string_view id_or_name) const;
    bool container_exists(std::string_view id_or_name) const;
    bool update_container_status(std::string_view id, std::string_view status, pid_t pid = -1, pid_t net_pid = -1) const;
    bool remove_container(std::string_view id) const;

    // Queries
    std::vector<ContainerState> list_containers(bool only_running = false) const;
    std::vector<ContainerState> list_containers_by_image(std::string_view image_ref) const;

    // Image Index Operations (~/.quiver/images/index.json)
    bool register_image(const ImageMetadata& img) const;
    std::optional<ImageMetadata> get_image(std::string_view name, std::string_view tag) const;
    bool remove_image(std::string_view name, std::string_view tag) const;
    std::vector<ImageMetadata> list_images() const;

    // Path Helpers
    fs::path container_bundle_dir(std::string_view id) const;
    fs::path container_state_file(std::string_view id) const;

private:
    fs::path m_base_dir;
    fs::path m_containers_dir;
    fs::path m_images_index_file;

    static bool atomic_write_json(const fs::path& target_path, const nlohmann::json& j);
    static std::string current_iso8601_time();
};
