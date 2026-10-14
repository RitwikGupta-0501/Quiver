#include "../include/image_manager.hpp"
#include "../include/utils.hpp"
#include <thread>
#include <chrono>
#include <iomanip>
#include <sys/stat.h>
#include <dirent.h>
#include <iostream>
#include <fstream>
#include <algorithm>

ImageManager::ImageManager(StateStore& store)
    : m_store(store) {
    const char* home_dir{ std::getenv("HOME") };
    if (home_dir != nullptr) {
        base_cache_path = std::string(home_dir) + "/.quiver/images";
    }
}

bool ImageManager::pull(const std::string& image_name, std::string& out_path, std::string& error) {
    std::string name, tag;
    size_t pos = image_name.find(':');
    if (pos != std::string::npos) {
        name = image_name.substr(0, pos);
        tag = image_name.substr(pos + 1);
        tag = tag.empty() ? "latest" : tag;
    } else {
        name = image_name;
        tag = "latest";
    }
    if(name.empty()){
        std::cerr << "Image name cannot be empty\n";
        return false;
    }
    if (base_cache_path.empty()) {
        error = "CRITICAL: HOME environment variable is not set.";
        return false;
    }

    ImageMetadata meta;
    meta.name = name;
    meta.tag = tag;

    if (!get_image(image_name, out_path, meta, error)) {
        return false;
    }

    meta.name = name;
    meta.tag = tag;
    meta.rootfs_path = out_path;
    if (!m_store.register_image(meta)) {
        error = "Failed to register image in state store: " + image_name;
        return false;
    }
    return true;
}

bool ImageManager::remove(const std::string& image_name, std::string& error) {
    if (base_cache_path.empty()) {
        error = "CRITICAL: HOME environment variable is not set.";
        return false;
    }
    std::string image_path = get_image_path(image_name);
    if (!Utils::path_exists(image_path)) {
        error = "Image not found locally: " + image_name;
        return false;
    }

    std::cout << "Removing image at " << image_path << '\n';
    if(Utils::remove_directory_recursively(image_path) == ERR){
        Utils::handle_error("Unable to remove " + image_name);
    }
    std::cout << "Image removed successfully." << '\n';
    return true;
}

bool ImageManager::get_image(const std::string& image_name, std::string& out_path, ImageMetadata& out_meta, std::string& error) {
    out_path = get_image_path(image_name);
    if (Utils::path_exists(out_path)) {
        std::cout << "Image found in local storage." << '\n';
        auto existing = m_store.get_image(out_meta.name, out_meta.tag);
        if (existing.has_value()) {
            out_meta = *existing;
        }
        return true;
    }

    std::cout << "Image not in local storage. Pulling from registry..." << '\n';
    if (!pull_image_from_registry(image_name, out_path, out_meta, error)) {
        if (Utils::path_exists(out_path)) {
            if(Utils::remove_directory_recursively(out_path) == ERR){
                Utils::handle_error("Unable to remove "+out_path);
            }
        }
        error = "Failed to pull image. Reason: " + error;
        return false;
    }
    return true;
}

bool ImageManager::pull_image_from_registry(const std::string& image_name, const std::string& image_path, ImageMetadata& out_meta, std::string& error) {
    std::string repo, tag;
    size_t colon_pos = image_name.find(':');
    repo = (colon_pos != std::string::npos) ? image_name.substr(0, colon_pos) : image_name;
    tag = (colon_pos != std::string::npos) ? image_name.substr(colon_pos + 1) : "latest";
    if (repo.find('/') == std::string::npos) repo = "library/" + repo;

    std::string token;
    if (!get_auth_token(image_name, token, error)) return false;

    json manifest;
    if (!get_manifest(image_name, token, manifest, error)) return false;

    const std::string host_arch = Utils::get_host_oci_arch();
    if (manifest.contains("manifests") && manifest["manifests"].is_array()) {
        std::string arch_digest;
        for (const auto& entry : manifest["manifests"]) {
            if (entry.contains("platform")) {
                const auto& plat = entry["platform"];
                std::string arch = plat.value("architecture", "");
                std::string os   = plat.value("os", "");
                if (arch == host_arch && os == "linux") {
                    arch_digest = entry.value("digest", "");
                    break;
                }
            }
        }
        if (arch_digest.empty()) {
            error = "No matching linux/" + host_arch + " manifest found in image index.";
            return false;
        }

        std::string arch_image_name = repo + "@" + arch_digest;
        if (!get_manifest(arch_image_name, token, manifest, error)) return false;
    }

    if (!fetch_and_parse_config(manifest, repo, token, out_meta, error)) {
        return false;
    }

    return download_and_extract_layers(manifest, repo, token, image_path, error);
}

bool ImageManager::fetch_and_parse_config(
    const json& manifest,
    const std::string& repo,
    const std::string& token,
    ImageMetadata& out_meta,
    std::string& error
) {
    if (!manifest.contains("config") || !manifest["config"].is_object()) {
        error = "Manifest missing config object.";
        return false;
    }
    std::string config_digest = manifest["config"].value("digest", "");
    std::uintmax_t config_size = manifest["config"].value("size", 0ULL);
    if (config_digest.empty()) {
        error = "Config descriptor missing digest.";
        return false;
    }

    fs::path tmp_config_path = fs::path(base_cache_path) / "config.json.tmp";
    Utils::ensure_dirs(tmp_config_path.parent_path());

    std::ofstream out_file(tmp_config_path, std::ios::binary);
    if (!out_file) {
        error = "Failed to open temporary file for config: " + tmp_config_path.string();
        return false;
    }

    std::string url = "https://registry-1.docker.io/v2/" + repo + "/blobs/" + config_digest;
    cpr::Response r = cpr::Download(out_file,
                                    cpr::Url{url},
                                    cpr::Header{{"Authorization", "Bearer " + token}});
    out_file.close();

    if (r.status_code != 200 && r.status_code != 307) {
        std::error_code ec;
        fs::remove(tmp_config_path, ec);
        error = "Failed to download image config. HTTP status: " + std::to_string(r.status_code);
        return false;
    }

    if (!Utils::verify_sha256_and_size(tmp_config_path, config_digest, config_size)) {
        std::error_code ec;
        fs::remove(tmp_config_path, ec);
        error = "Config blob integrity check failed for " + config_digest;
        return false;
    }

    std::ifstream in_file(tmp_config_path);
    if (!in_file) {
        std::error_code ec;
        fs::remove(tmp_config_path, ec);
        error = "Failed to open config JSON file.";
        return false;
    }

    json config_json;
    try {
        in_file >> config_json;
    } catch (const std::exception& e) {
        std::error_code ec;
        fs::remove(tmp_config_path, ec);
        error = "Failed to parse config JSON: " + std::string(e.what());
        return false;
    }
    in_file.close();
    std::error_code ec;
    fs::remove(tmp_config_path, ec);

    json cfg = config_json.value("config", json::object());
    if (cfg.contains("Env") && cfg["Env"].is_array()) {
        out_meta.default_env.clear();
        for (const auto& env_item : cfg["Env"]) {
            if (env_item.is_string()) {
                out_meta.default_env.push_back(env_item.get<std::string>());
            }
        }
    }
    if (cfg.contains("Entrypoint") && cfg["Entrypoint"].is_array()) {
        out_meta.default_entrypoint.clear();
        for (const auto& ep : cfg["Entrypoint"]) {
            if (ep.is_string()) {
                out_meta.default_entrypoint.push_back(ep.get<std::string>());
            }
        }
    }
    if (cfg.contains("Cmd") && cfg["Cmd"].is_array()) {
        out_meta.default_cmd.clear();
        for (const auto& cmd_item : cfg["Cmd"]) {
            if (cmd_item.is_string()) {
                out_meta.default_cmd.push_back(cmd_item.get<std::string>());
            }
        }
    }
    if (cfg.contains("WorkingDir") && cfg["WorkingDir"].is_string()) {
        std::string wd = cfg["WorkingDir"].get<std::string>();
        if (!wd.empty()) {
            out_meta.working_dir = wd;
        }
    }

    return true;
}

bool ImageManager::get_manifest(const std::string& image_name, const std::string& token, json& out_manifest, std::string& error) {
    std::string repo, reference;
    size_t at_pos = image_name.find('@');
    if (at_pos != std::string::npos) {
        repo = image_name.substr(0, at_pos);
        reference = image_name.substr(at_pos + 1);
    } else {
        size_t colon_pos = image_name.find(':');
        repo = (colon_pos != std::string::npos) ? image_name.substr(0, colon_pos) : image_name;
        reference = (colon_pos != std::string::npos) ? image_name.substr(colon_pos + 1) : "latest";
    }
    if (repo.find('/') == std::string::npos) repo = "library/" + repo;

    cpr::Url manifest_url{"https://registry-1.docker.io/v2/" + repo + "/manifests/" + reference};
    cpr::Response r = cpr::Get(manifest_url, cpr::Header{
        {"Authorization", "Bearer " + token},
        {"Accept",
         "application/vnd.oci.image.manifest.v1+json, "
         "application/vnd.oci.image.index.v1+json, "
         "application/vnd.docker.distribution.manifest.v2+json, "
         "application/vnd.docker.distribution.manifest.list.v2+json"}
    });
    if (r.status_code != 200) {
        error = "Failed to get manifest: " + std::to_string(r.status_code) + " " + r.reason;
        return false;
    }
    out_manifest = json::parse(r.text);
    return true;
}

bool ImageManager::get_auth_token(const std::string& image_name, std::string& out_token, std::string& error) {
    std::string repo;
    size_t colon_pos = image_name.find(':');
    repo = (colon_pos != std::string::npos) ? image_name.substr(0, colon_pos) : image_name;
    if (repo.find('/') == std::string::npos) repo = "library/" + repo;

    cpr::Response r = cpr::Get(cpr::Url{"https://registry-1.docker.io/v2/"});
    if (r.header.find("Www-Authenticate") == r.header.end()) {
        error = "Could not find Www-Authenticate header.";
        return false;
    }

    std::string auth_header = r.header["Www-Authenticate"];
    std::string realm = parse_auth_header(auth_header, "realm");
    std::string service = parse_auth_header(auth_header, "service");
    if (realm.empty() || service.empty()) {
        error = "Failed to parse realm or service from auth header.";
        return false;
    }

    cpr::Url auth_url{realm};
    cpr::Parameters params{{"service", service}, {"scope", "repository:" + repo + ":pull"}};
    cpr::Response token_response = cpr::Get(auth_url, params);
    if (token_response.status_code != 200) {
        error = "Failed to get auth token: " + std::to_string(token_response.status_code) + " - " + token_response.text;
        return false;
    }

    json token_json = json::parse(token_response.text);
    if (!token_json.contains("token")) {
        error = "Auth token response did not contain a 'token' field.";
        return false;
    }
    out_token = token_json["token"];
    return true;
}

std::string ImageManager::parse_auth_header(const std::string& header, const std::string& key) {
    size_t key_pos = header.find(key + "=\"");
    if (key_pos == std::string::npos) return "";
    size_t value_start = key_pos + key.length() + 2;
    size_t value_end = header.find("\"", value_start);
    if (value_end == std::string::npos) return "";
    return header.substr(value_start, value_end - value_start);
}

std::future<std::string> ImageManager::download_layer_async(const std::string& url, const std::string& token, const std::string& destination_path, const std::string& digest) {
    return std::async(std::launch::async, [this, url, token, destination_path, digest]() -> std::string {
        std::ofstream out_file(destination_path, std::ios::binary);
        if (!out_file) {
            return "Failed to open file for writing: " + destination_path;
        }

        auto callback = cpr::ProgressCallback([&](cpr::cpr_off_t downloadTotal, cpr::cpr_off_t downloadNow, cpr::cpr_off_t, cpr::cpr_off_t, intptr_t) -> bool {
            std::lock_guard<std::mutex> lock(m_progress_mutex);
            m_download_progress[digest] = {downloadTotal, downloadNow};
            return true;
        });

        cpr::Response r = cpr::Download(out_file,
                                        cpr::Url{url},
                                        cpr::Header{{"Authorization", "Bearer " + token}},
                                        callback);
        out_file.close();

        if (r.status_code != 200 && r.status_code != 307) {
            return "Failed to download layer to " + destination_path + ". Status: " + std::to_string(r.status_code);
        }
        return "";
    });
}

void ImageManager::print_progress() {
    bool downloads_finished = false;
    while (!downloads_finished) {
        long long total_downloaded = 0;
        long long total_size = 0;
        int completed_layers = 0;
        int total_layers = 0;

        {
            std::lock_guard<std::mutex> lock(m_progress_mutex);
            total_layers = m_download_progress.size();
            if (total_layers == 0) continue;

            for (const auto& pair : m_download_progress) {
                total_downloaded += pair.second.downloaded;
                total_size += pair.second.total;
                if (pair.second.downloaded == pair.second.total && pair.second.total > 0) {
                    completed_layers++;
                }
            }
        }

        if (total_layers > 0) {
            float percentage = (total_size > 0) ? (static_cast<float>(total_downloaded) / total_size) * 100.0f : 0.0f;
            int bar_width = 50;
            int pos = bar_width * (percentage / 100.0f);

            std::cout << "\rDownloading: [";
            for (int i=0; i < bar_width; ++i) {
                if (i < pos) std::cout << "=";
                else if (i == pos) std::cout << ">";
                else std::cout << " ";
            }
            std::cout << "] " << std::fixed << std::setprecision(2) << percentage << "% ";
            std::cout << "(" << completed_layers << "/" << total_layers << " layers) ";
            std::cout << "(" << (total_downloaded / 1024 / 1024) << "MB / " << (total_size / 1024 / 1024) << "MB)";
            std::cout.flush();
        }

        if (completed_layers == total_layers && total_layers > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << '\n'; // Clean up with a final newline after the loop finishes.
}

bool ImageManager::apply_layer_with_whiteouts(
    const fs::path& tarball_path,
    const fs::path& destination_path,
    size_t layer_index,
    Utils::ArchiveCompression compression,
    std::string& error
) {
    fs::path staging_dir = destination_path.parent_path() / (".layer_staging_" + std::to_string(layer_index));
    std::error_code ec;
    fs::remove_all(staging_dir, ec);
    Utils::ensure_dirs(staging_dir);

    if (!Utils::extract_tarball(tarball_path, staging_dir, compression)) {
        fs::remove_all(staging_dir, ec);
        error = "Failed to extract layer tarball into staging directory: " + tarball_path.string();
        return false;
    }

    // First Pass: Opaque Whiteouts (.wh..wh..opq)
    std::vector<fs::path> opaque_whiteouts;
    for (auto it = fs::recursive_directory_iterator(staging_dir, fs::directory_options::skip_permission_denied);
         it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file() && it->path().filename() == ".wh..wh..opq") {
            opaque_whiteouts.push_back(it->path());
        }
    }
    for (const auto& opq_path : opaque_whiteouts) {
        fs::path rel_parent = fs::relative(opq_path.parent_path(), staging_dir);
        fs::path target_parent = (rel_parent == ".") ? destination_path : (destination_path / rel_parent);
        if (fs::exists(target_parent) && fs::is_directory(target_parent)) {
            for (const auto& entry : fs::directory_iterator(target_parent, fs::directory_options::skip_permission_denied)) {
                fs::remove_all(entry.path(), ec);
            }
        }
        fs::remove(opq_path, ec);
    }

    // Second Pass: Explicit Whiteouts (.wh.<filename>)
    std::vector<fs::path> explicit_whiteouts;
    for (auto it = fs::recursive_directory_iterator(staging_dir, fs::directory_options::skip_permission_denied);
         it != fs::recursive_directory_iterator(); ++it) {
        std::string filename = it->path().filename().string();
        if (filename.starts_with(".wh.") && filename != ".wh..wh..opq") {
            explicit_whiteouts.push_back(it->path());
        }
    }
    for (const auto& wh_path : explicit_whiteouts) {
        std::string filename = wh_path.filename().string();
        std::string target_name = filename.substr(4);
        fs::path rel_parent = fs::relative(wh_path.parent_path(), staging_dir);
        fs::path target_item = (rel_parent == ".") ? (destination_path / target_name) : (destination_path / rel_parent / target_name);
        fs::remove_all(target_item, ec);
        fs::remove(wh_path, ec);
    }

    // Third Pass: Merge remaining files and directories
    std::vector<fs::path> dirs_to_create;
    std::vector<fs::path> items_to_copy;
    for (auto it = fs::recursive_directory_iterator(staging_dir, fs::directory_options::skip_permission_denied);
         it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_directory()) {
            dirs_to_create.push_back(it->path());
        } else {
            items_to_copy.push_back(it->path());
        }
    }
    for (const auto& d : dirs_to_create) {
        fs::path rel = fs::relative(d, staging_dir);
        Utils::ensure_dirs(destination_path / rel);
    }
    for (const auto& item : items_to_copy) {
        fs::path rel = fs::relative(item, staging_dir);
        fs::path dest = destination_path / rel;
        Utils::ensure_dirs(dest.parent_path());
        fs::remove_all(dest, ec);
        fs::copy(item, dest, fs::copy_options::copy_symlinks | fs::copy_options::overwrite_existing, ec);
    }

    fs::remove_all(staging_dir, ec);
    return true;
}

bool ImageManager::extract_layer(const std::string& tarball_path, const std::string& destination_path, std::string& error) {
    return apply_layer_with_whiteouts(tarball_path, destination_path, 0, Utils::ArchiveCompression::Auto, error);
}

bool ImageManager::download_and_extract_layers(const json& manifest, const std::string& repo, const std::string& token, const std::string& destination_path, std::string& error) {
    Utils::ensure_dirs(destination_path);

    if (!manifest.contains("layers") || manifest["layers"].empty()) {
        error = "Manifest does not contain any 'layers'.";
        return false;
    }

    std::vector<std::string> digests;
    std::vector<std::uintmax_t> sizes;
    std::vector<std::string> media_types;
    for (const auto& layer : manifest["layers"]) {
        std::string d = layer["digest"].get<std::string>();
        digests.push_back(d);
        sizes.push_back(layer.value("size", 0ULL));
        media_types.push_back(layer.value("mediaType", ""));
        m_download_progress[d] = {0, 0};
    }

    std::vector<std::future<std::string>> download_futures;
    std::vector<std::string> tarball_paths;

    for (size_t i = 0; i < digests.size(); ++i) {
        const auto& digest = digests[i];
        std::string url = "https://registry-1.docker.io/v2/" + repo + "/blobs/" + digest;
        std::string raw_digest = (digest.starts_with("sha256:")) ? digest.substr(7) : digest;
        std::string path = destination_path + "/.layer_" + raw_digest + ".tar";
        tarball_paths.push_back(path);
        download_futures.push_back(download_layer_async(url, token, path, digest));
    }

    std::thread progress_thread(&ImageManager::print_progress, this);

    for (size_t i = 0; i < digests.size(); ++i) {
        std::string download_error = download_futures[i].get();
        if (!download_error.empty()) {
            error = download_error;
            progress_thread.join();
            for (const auto& p : tarball_paths) {
                std::error_code ec;
                fs::remove(p, ec);
            }
            return false;
        }
    }

    progress_thread.join();

    std::cout << "Download complete. Verifying layer digests..." << '\n';
    for (size_t i = 0; i < digests.size(); ++i) {
        if (!Utils::verify_sha256_and_size(tarball_paths[i], digests[i], sizes[i])) {
            error = "Layer digest or size verification failed for " + digests[i];
            for (const auto& p : tarball_paths) {
                std::error_code ec;
                fs::remove(p, ec);
            }
            return false;
        }
    }

    std::cout << "Layers verified. Now extracting layers with whiteout processing..." << '\n';
    for (size_t i = 0; i < digests.size(); ++i) {
        std::cout << "  -> Extracting layer " << i+1 << "/" << digests.size() << " (" << digests[i].substr(0, 12) << ")" << '\n';
        Utils::ArchiveCompression comp = Utils::ArchiveCompression::Auto;
        const auto& mt = media_types[i];
        if (mt.ends_with("+gzip") || mt.ends_with(".gzip")) {
            comp = Utils::ArchiveCompression::Gzip;
        } else if (mt.ends_with("+zstd") || mt.ends_with(".zstd")) {
            comp = Utils::ArchiveCompression::Zstd;
        } else if (mt.ends_with(".tar")) {
            comp = Utils::ArchiveCompression::Uncompressed;
        }

        if (!apply_layer_with_whiteouts(tarball_paths[i], destination_path, i, comp, error)) {
            for (const auto& p : tarball_paths) {
                std::error_code ec;
                fs::remove(p, ec);
            }
            return false;
        }
        std::error_code ec;
        fs::remove(tarball_paths[i], ec);
    }
    std::cout << "Image pull and extraction complete." << '\n';
    return true;
}

std::string ImageManager::get_image_path(const std::string& image_name) const {
    std::string safe_image_name = image_name;
    std::replace(safe_image_name.begin(), safe_image_name.end(), ':', '_');
    std::replace(safe_image_name.begin(), safe_image_name.end(), '/', '_');
    return this->base_cache_path + "/" + safe_image_name;
}

ImageManager::~ImageManager() {}
