#pragma once
#include <vector>
#include <string>
#include "state_store.hpp"
#include "image_manager.hpp"

class CommandLineHandler {
public:
    explicit CommandLineHandler(StateStore& store, ImageManager& img_manager);
    ~CommandLineHandler() = default;

    void run(const std::vector<std::string>& args);
    void attach(const std::vector<std::string>& args);
    void ps(const std::vector<std::string>& args);
    void rm(const std::vector<std::string>& args);
    void image(const std::vector<std::string>& args);
    void volume(const std::vector<std::string>& args);
    void network(const std::vector<std::string>& args);
    void create(const std::vector<std::string>& args);
    void pull(const std::vector<std::string>& args);
    void start(const std::vector<std::string>& args);
    void stop(const std::vector<std::string>& args);
    void vfs(const std::vector<std::string>& args);

private:
    StateStore& m_store;
    ImageManager& m_img_manager;
};
