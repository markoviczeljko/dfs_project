// Entry point for the dfs_master executable (see docs/design.md §10).
// Usage: dfs_master --config <path>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "master/master.h"

namespace {

std::atomic<bool> g_shutdown_requested{false};

void handle_shutdown_signal(int) { g_shutdown_requested.store(true); }

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// configs/master.conf is a plain key=value text file — no schema for it
// exists in the design notes, same invention as configs/storageN.conf.
Result<Master::Config> load_config(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return Result<Master::Config>::err(ErrorCode::INVALID_REQUEST);
    }

    Master::Config config{};
    bool has_port = false;
    bool has_metadata_dir = false;

    auto parse_port = [](const std::string& value, uint16_t& out) {
        size_t consumed = 0;
        unsigned long parsed = std::stoul(value, &consumed);
        if (consumed != value.size() || parsed > 65535) {
            throw std::invalid_argument("port out of range");
        }
        out = static_cast<uint16_t>(parsed);
    };

    // Faza 17.1: replication_factor must be at least 1 — RF=0 would mean
    // no replicas ever chosen, silently breaking every ALLOCATE_BLOCK.
    auto parse_replication_factor = [](const std::string& value, uint32_t& out) {
        size_t consumed = 0;
        unsigned long parsed = std::stoul(value, &consumed);
        if (consumed != value.size() || parsed == 0 || parsed > 0xFFFFFFFFUL) {
            throw std::invalid_argument("replication_factor out of range");
        }
        out = static_cast<uint32_t>(parsed);
    };

    std::string line;
    try {
        while (std::getline(file, line)) {
            std::string trimmed = trim(line);
            if (trimmed.empty() || trimmed[0] == '#') {
                continue;
            }

            size_t eq = trimmed.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            std::string key = trim(trimmed.substr(0, eq));
            std::string value = trim(trimmed.substr(eq + 1));

            if (key == "port") {
                parse_port(value, config.port);
                has_port = true;
            } else if (key == "metadata_dir") {
                config.metadata_dir = value;
                has_metadata_dir = true;
            } else if (key == "replication_factor") {
                parse_replication_factor(value, config.replication_factor);
            }
        }
    } catch (const std::exception&) {
        return Result<Master::Config>::err(ErrorCode::INVALID_REQUEST);
    }

    if (!has_port || !has_metadata_dir) {
        return Result<Master::Config>::err(ErrorCode::INVALID_REQUEST);
    }
    return Result<Master::Config>::ok(config);
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    if (config_path.empty()) {
        std::cerr << "Usage: " << argv[0] << " --config <path>\n";
        return 1;
    }

    auto config_result = load_config(config_path);
    if (config_result.is_error()) {
        std::cerr << "Failed to load config file: " << config_path << "\n";
        return 1;
    }

    const Master::Config& config = config_result.value();
    auto master_result = Master::create(config);
    if (master_result.is_error()) {
        std::cerr << "Failed to initialize master\n";
        return 1;
    }

    auto master = std::move(master_result.value());
    if (master->start() != ErrorCode::OK) {
        std::cerr << "Failed to start master on port " << config.port << "\n";
        return 1;
    }

    std::signal(SIGINT, handle_shutdown_signal);
    std::signal(SIGTERM, handle_shutdown_signal);

    std::cout << "dfs_master: listening on port " << config.port << "\n";

    while (!g_shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "dfs_master: shutting down\n";
    master->stop();
    return 0;
}
