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

    if (!get_image(image_name, out_path, error)) {
        return false;
    }

    if(!m_store.get_image(name, tag).has_value()){
        ImageMetadata meta;
        meta.name = name;
        meta.tag = tag;
        meta.rootfs_path = out_path;
        if (!m_store.register_image(meta)) {
            error = "Failed to register image in state store: " + image_name;
            return false;
        }
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

bool ImageManager::get_image(const std::string& image_name, std::string& out_path, std::string& error) {
    out_path = get_image_path(image_name);
    if (Utils::path_exists(out_path)) {
        std::cout << "Image found in local storage." << '\n';
        return true;
    }

    std::cout << "Image not in local storage. Pulling from registry..." << '\n';
    if (!pull_image_from_registry(image_name, out_path, error)) {
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

bool ImageManager::pull_image_from_registry(const std::string& image_name, const std::string& image_path, std::string& error) {
    std::string repo, tag;
    size_t colon_pos = image_name.find(':');
    repo = (colon_pos != std::string::npos) ? image_name.substr(0, colon_pos) : image_name;
    tag = (colon_pos != std::string::npos) ? image_name.substr(colon_pos + 1) : "latest";
    if (repo.find('/') == std::string::npos) repo = "library/" + repo;

    std::string token;
    if (!get_auth_token(image_name, token, error)) return false;

    json manifest;
    if (!get_manifest(image_name, token, manifest, error)) return false;

    if (manifest.contains("manifests")) {
        std::string arch_digest;
        for (const auto& entry : manifest["manifests"]) {
            if (entry.contains("platform") && entry["platform"]["architecture"] == "amd64") {
                arch_digest = entry["digest"];
                break;
            }
        }
        if (arch_digest.empty()) {
            error = "Could not find an 'amd64' manifest.";
            return false;
        }

        std::string arch_image_name = repo + "@" + arch_digest;
        if (!get_manifest(arch_image_name, token, manifest, error)) return false;
    }

    return download_and_extract_layers(manifest, repo, token, image_path, error);
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
        {"Accept", "application/vnd.docker.distribution.manifest.v2+json, application/vnd.oci.image.index.v1+json"}
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

bool ImageManager::extract_layer(const std::string& tarball_path, const std::string& destination_path, std::string& error) {
    if(!Utils::extract_tarball(tarball_path, destination_path)){
        error = "Unable to extract layer " + tarball_path;
        return false;
    }
    return true;
}

bool ImageManager::download_and_extract_layers(const json& manifest, const std::string& repo, const std::string& token, const std::string& destination_path, std::string& error) {
    Utils::ensure_dirs(destination_path);

    if (!manifest.contains("layers") || manifest["layers"].empty()) {
        error = "Manifest does not contain any 'layers'.";
        return false;
    }

    std::vector<std::string> digests;
    for (const auto& layer : manifest["layers"]) {
        digests.push_back(layer["digest"]);
        m_download_progress[layer["digest"].get<std::string>()] = {0, 0};
    }

    std::vector<std::future<std::string>> download_futures;
    std::vector<std::string> tarball_paths;

    for (const auto& digest : digests) {
        std::string url = "https://registry-1.docker.io/v2/" + repo + "/blobs/" + digest;
        std::string path = destination_path + "/" + digest.substr(7) + ".tar.gz";
        tarball_paths.push_back(path);
        download_futures.push_back(download_layer_async(url, token, path, digest));
    }

    std::thread progress_thread(&ImageManager::print_progress, this);

    for (size_t i = 0; i < digests.size(); ++i) {
        std::string download_error = download_futures[i].get();
        if (!download_error.empty()) {
            error = download_error;
            progress_thread.join();
            return false;
        }
    }

    progress_thread.join();

    std::cout << "Download complete. Now extracting layers..." << '\n';
    for (size_t i = 0; i < digests.size(); ++i) {
        std::cout << "  -> Extracting layer " << i+1 << "/" << digests.size() << " (" << digests[i].substr(0, 12) << ")" << '\n';
        if (!extract_layer(tarball_paths[i], destination_path, error)) {
            return false;
        }
        std::remove(tarball_paths[i].c_str());
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
