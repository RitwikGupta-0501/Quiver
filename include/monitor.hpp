#pragma once
#include <string>
#include <string_view>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <filesystem>
#include "singleton.hpp"

namespace fs = std::filesystem;

class Monitor : public Singleton<Monitor> {
    friend class Singleton<Monitor>;
private:
    Monitor();
    ~Monitor();
public:
    auto set_log_path(const fs::path& path) -> void;
    auto log(std::string_view level, std::string_view component, std::string_view message) -> void;
    auto log(std::string_view message) -> void;
    auto log(std::string message) -> void;

private:
    std::thread m_worker{};
    std::queue<std::string> m_queue{};
    std::mutex m_mtx{};
    std::condition_variable m_cv{};
    std::atomic<bool> m_running{true};
    pid_t m_owner_pid{-1};
    fs::path m_log_path{};
};
