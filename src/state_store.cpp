#include "state_store.hpp"
#include "utils.hpp"
#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <unistd.h>
#include <csignal>
#include <iostream>

StateStore::StateStore(fs::path base_dir)
    : m_base_dir(std::move(base_dir))
    , m_containers_dir(m_base_dir / "containers")
    , m_images_index_file(m_base_dir / "images" / "index.json") {
    ensure_store_layout();
}

void StateStore::ensure_store_layout() const {
    Utils::ensure_dirs(m_containers_dir);
    Utils::ensure_dirs(m_base_dir / "images");
    Utils::ensure_dirs(m_base_dir / "logs");
    if (!fs::exists(m_images_index_file)) {
        atomic_write_json(m_images_index_file, nlohmann::json::array());
    }
}

bool StateStore::atomic_write_json(const fs::path& target_path, const nlohmann::json& j) {
    try {
        Utils::ensure_dirs(target_path.parent_path());
        fs::path tmp_path = target_path;
        tmp_path += ".tmp." + std::to_string(::getpid());

        {
            std::ofstream ofs(tmp_path, std::ios::out | std::ios::trunc);
            if (!ofs.is_open()) return false;
            ofs << j.dump(4) << '\n';
            ofs.flush();
            if (!ofs.good()) return false;
        }
        fs::rename(tmp_path, target_path);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "StateStore error writing " << target_path << ": " << e.what() << '\n';
        return false;
    }
}

std::string StateStore::current_iso8601_time() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

fs::path StateStore::container_bundle_dir(std::string_view id) const {
    return m_containers_dir / id;
}

fs::path StateStore::container_state_file(std::string_view id) const {
    return container_bundle_dir(id) / "state.json";
}

bool StateStore::save_container(const ContainerState& state) const {
    ContainerState copy = state;
    if (copy.created_at.empty()) {
        copy.created_at = current_iso8601_time();
    }
    if (copy.bundle_path.empty()) {
        copy.bundle_path = container_bundle_dir(copy.id).string();
    }
    return atomic_write_json(container_state_file(copy.id), nlohmann::json(copy));
}

std::optional<ContainerState> StateStore::get_container(std::string_view id_or_name) const {
    if (id_or_name.empty()) return std::nullopt;

    // 1. Fast path: Exact ID match
    fs::path exact_path = container_state_file(id_or_name);
    if (fs::exists(exact_path)) {
        try {
            std::ifstream ifs(exact_path);
            return nlohmann::json::parse(ifs).get<ContainerState>();
        } catch (...) {
            return std::nullopt;
        }
    }

    // 2. Fallback: Match by container name or ID prefix (like Docker)
    for (const auto& c : list_containers(false)) {
        if (c.name == id_or_name || c.id.starts_with(id_or_name)) {
            return c;
        }
    }
    return std::nullopt;
}

bool StateStore::container_exists(std::string_view id_or_name) const {
    return get_container(id_or_name).has_value();
}

bool StateStore::update_container_status(std::string_view id, std::string_view status, pid_t pid, pid_t net_pid) const {
    auto opt = get_container(id);
    if (!opt) return false;

    ContainerState state = *opt;
    state.status = std::string(status);
    if (pid > 0) state.pid = pid;
    if (net_pid > 0) state.net_pid = net_pid;

    // Verify if process is actually alive when querying/updating
    return save_container(state);
}

bool StateStore::remove_container(std::string_view id_or_name) const {
    auto opt = get_container(id_or_name);
    if (!opt) return false;
    std::error_code ec;
    fs::remove_all(container_bundle_dir(opt->id), ec);
    return !ec;
}

std::vector<ContainerState> StateStore::list_containers(bool only_running) const {
    std::vector<ContainerState> results;
    if (!fs::exists(m_containers_dir)) return results;

    for (const auto& entry : fs::directory_iterator(m_containers_dir)) {
        if (!entry.is_directory()) continue;
        fs::path state_file = entry.path() / "state.json";
        if (!fs::exists(state_file)) continue;

        try {
            std::ifstream ifs(state_file);
            auto state = nlohmann::json::parse(ifs).get<ContainerState>();

            // Auto-heal stale "running" status if PID no longer exists in kernel
            if (state.status == "running" && state.pid > 0) {
                if (::kill(state.pid, 0) != 0 && errno == ESRCH) {
                    state.status = "exited";
                    atomic_write_json(state_file, nlohmann::json(state));
                }
            }

            if (!only_running || state.status == "running") {
                results.push_back(std::move(state));
            }
        } catch (...) {
            continue;
        }
    }
    return results;
}

std::vector<ContainerState> StateStore::list_containers_by_image(std::string_view image_ref) const {
    std::vector<ContainerState> matches;
    for (const auto& c : list_containers(false)) {
        if (c.image == image_ref) {
            matches.push_back(c);
        }
    }
    return matches;
}

bool StateStore::register_image(const ImageMetadata& img) const {
    auto images = list_images();
    std::erase_if(images, [&](const ImageMetadata& existing) {
        return existing.name == img.name && existing.tag == img.tag;
    });

    ImageMetadata copy = img;
    if (copy.created_at.empty()) copy.created_at = current_iso8601_time();
    images.push_back(std::move(copy));

    return atomic_write_json(m_images_index_file, nlohmann::json(images));
}

std::optional<ImageMetadata> StateStore::get_image(std::string_view name, std::string_view tag) const {
    for (const auto& img : list_images()) {
        if (img.name == name && img.tag == tag) return img;
    }
    return std::nullopt;
}

bool StateStore::remove_image(std::string_view name, std::string_view tag) const {
    auto images = list_images();
    auto prev_size = images.size();
    std::erase_if(images, [&](const ImageMetadata& existing) {
        return existing.name == name && existing.tag == tag;
    });
    if (images.size() == prev_size) return false;
    return atomic_write_json(m_images_index_file, nlohmann::json(images));
}

std::vector<ImageMetadata> StateStore::list_images() const {
    if (!fs::exists(m_images_index_file)) return {};
    try {
        std::ifstream ifs(m_images_index_file);
        return nlohmann::json::parse(ifs).get<std::vector<ImageMetadata>>();
    } catch (...) {
        return {};
    }
}
