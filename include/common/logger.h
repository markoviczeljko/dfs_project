#ifndef DFS_COMMON_LOGGER_H_
#define DFS_COMMON_LOGGER_H_

#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string_view>

// Faza 14: minimal logging, deliberately separate from StatisticsManager
// (see docs/design.md §18, design notes §10.31-§10.32 — "logging answers
// WHAT happened, statistics answer HOW OFTEN/HOW LONG"). Header-only so
// `common` stays an INTERFACE library — no new .cpp/library restructuring
// needed for something this small (C++17 inline variables avoid ODR
// issues across translation units).
namespace logger {

enum class LogLevel { DEBUG, INFO, WARNING, ERROR };

inline std::atomic<LogLevel> g_log_level{LogLevel::INFO};
inline std::mutex g_log_mutex;  // std::cerr itself isn't safe under concurrent <<-chains

inline void set_level(LogLevel level) {
    g_log_level.store(level, std::memory_order_relaxed);
}

inline std::string_view level_name(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::INFO:
            return "INFO";
        case LogLevel::WARNING:
            return "WARNING";
        case LogLevel::ERROR:
            return "ERROR";
    }
    return "UNKNOWN";
}

inline void log(LogLevel level, std::string_view component, std::string_view message) {
    if (level < g_log_level.load(std::memory_order_relaxed)) {
        return;
    }
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm_buf{};
    localtime_r(&now, &tm_buf);

    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cerr << "[" << std::put_time(&tm_buf, "%H:%M:%S") << "] " << level_name(level) << " ["
              << component << "] " << message << "\n";
}

}  // namespace logger

#endif  // DFS_COMMON_LOGGER_H_
