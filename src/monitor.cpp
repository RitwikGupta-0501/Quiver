#include <iostream>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <mutex>
#include <queue>
#include <thread>
#include <format>
#include <ctime>
#include <unistd.h>
#include "utils.hpp"
#include "singleton.hpp"
#include "monitor.hpp"

Monitor::Monitor() {
    m_owner_pid = ::getpid();
    m_running.store(true);
    m_worker = std::thread([this]() -> void {
        while (true) {
            std::string current_buffer;
            fs::path current_path;
            {
                std::unique_lock<std::mutex> lock(m_mtx);
                m_cv.wait(lock, [this]() -> bool {
                    return !m_running.load() || !m_queue.empty();
                });
                if (!m_running.load() && m_queue.empty()) {
                    break;
                }
                current_buffer = std::move(m_queue.front());
                m_queue.pop();
                if (m_log_path.empty()) [[unlikely]] {
                    m_log_path = Utils::get_base_dir() + "/quiver.log";
                    try { Utils::ensure_file(m_log_path); } catch (...) {}
                }
                current_path = m_log_path;
            }
            if (!current_path.empty()) {
                try {
                    Utils::append_file(current_path, current_buffer);
                }
                catch (const std::exception& e) {
                    std::cerr << "Monitor write error: " << e.what() << '\n';
                }
            }
        }
    });
}

Monitor::~Monitor() {
    if (m_owner_pid != ::getpid()) {
        if (m_worker.joinable()) {
            m_worker.detach();
        }
        return;
    }
    m_running.store(false);
    m_cv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

auto Monitor::set_log_path(const fs::path& log_path) -> void {
    std::lock_guard<std::mutex> lock(m_mtx);
    m_log_path = log_path;
    try {
        Utils::ensure_file(log_path);
    }
    catch (const std::exception& e) {
        std::cerr << "Monitor set_log_path error: " << e.what() << '\n';
    }
}

auto Monitor::log(std::string_view level, std::string_view component, std::string_view message) -> void {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    gmtime_r(&time_t_now, &tm_buf);
    char time_str[32];
    std::strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

    std::string formatted = std::format("[{}] [PID {}] [{}] [{}] {}\n",
        time_str, ::getpid(), level, component, message);

    {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_queue.push(std::move(formatted));
    }
    m_cv.notify_one();
}

auto Monitor::log(std::string_view message) -> void {
    log("INFO", "Quiver", message);
}

auto Monitor::log(std::string message) -> void {
    log("INFO", "Quiver", std::string_view(message));
}
