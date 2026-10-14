#pragma once

#include <string>
#include <vector>
#include <future>
#include <mutex>
#include <map>
#include <nlohmann/json.hpp>
#include <cpr/cpr.h>
#include "state_store.hpp"
#include "utils.hpp"

using json = nlohmann::json;

class ImageManager {
    public:
    explicit ImageManager(StateStore& store);
    ~ImageManager();

    // Public API
    bool pull(const std::string& imageName, std::string& outPath, std::string& error);
    bool remove(const std::string& imageName, std::string& error);

    private:
        StateStore& m_store;
        std::string base_cache_path;

        struct DownloadProgress {
            long long total = 0;
            long long downloaded = 0;
        };
        std::map<std::string, DownloadProgress> m_download_progress;
        std::mutex m_progress_mutex;

        // Helper Functions
        bool get_image(const std::string& imageName, std::string& outPath, ImageMetadata& outMeta, std::string& error);
        bool pull_image_from_registry(const std::string& imageName, const std::string& imagePath, ImageMetadata& outMeta, std::string& error);
        bool get_manifest(const std::string& imageName, const std::string& token, json& outManifest, std::string& error);
        bool get_auth_token(const std::string& imageName, std::string& outToken, std::string& error);
        bool fetch_and_parse_config(const json& manifest, const std::string& repo, const std::string& token, ImageMetadata& outMeta, std::string& error);
        bool download_and_extract_layers(const json& manifest, const std::string& repo, const std::string& token, const std::string& destinationPath, std::string& error);
        bool apply_layer_with_whiteouts(const fs::path& tarball_path, const fs::path& destination_path, size_t layer_index, Utils::ArchiveCompression compression, std::string& error);
        bool extract_layer(const std::string& tarball_path, const std::string& destination_path, std::string& error);

        std::future<std::string> download_layer_async(const std::string& url, const std::string& token, const std::string& destination_path, const std::string& digest);
        void print_progress();

        std::string get_image_path(const std::string& imageName) const;
        bool path_exists(const std::string& path) const;
        std::string parse_auth_header(const std::string& header, const std::string& key);
};
