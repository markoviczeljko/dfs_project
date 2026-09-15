// Entry point for the dfs_storage executable (see docs/design.md §9).
// Usage: dfs_storage --config <path>

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "storage/storage_node.h"

namespace {

std::atomic<bool> g_shutdown_requested{false};

void handle_shutdown_signal(int) { g_shutdown_requested.store(true); }

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// configs/storageN.conf is a plain key=value text file — no schema for it
// exists in the design notes, so this format is this phase's own invention.
Result<StorageNode::Config> load_config(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return Result<StorageNode::Config>::err(ErrorCode::INVALID_REQUEST);
    }

    StorageNode::Config config{};
    bool has_node_id = false;
    bool has_port = false;
    bool has_data_dir = false;
    bool has_capacity = false;

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

            if (key == "node_id") {
                config.node_id = std::stoull(value);
                has_node_id = true;
            } else if (key == "port") {
                config.port = static_cast<uint16_t>(std::stoul(value));
                has_port = true;
            } else if (key == "data_dir") {
                config.data_dir = value;
                has_data_dir = true;
            } else if (key == "capacity_bytes") {
                config.capacity_bytes = std::stoull(value);
                has_capacity = true;
            }
        }
    } catch (const std::exception&) {
        return Result<StorageNode::Config>::err(ErrorCode::INVALID_REQUEST);
    }

    if (!has_node_id || !has_port || !has_data_dir || !has_capacity) {
        return Result<StorageNode::Config>::err(ErrorCode::INVALID_REQUEST);
    }
    return Result<StorageNode::Config>::ok(config);
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

    const StorageNode::Config& config = config_result.value();
    auto node_result = StorageNode::create(config);
    if (node_result.is_error()) {
        std::cerr << "Failed to initialize storage node (data_dir=" << config.data_dir << ")\n";
        return 1;
    }

    auto node = std::move(node_result.value());
    if (node->start() != ErrorCode::OK) {
        std::cerr << "Failed to start storage node on port " << config.port << "\n";
        return 1;
    }

    std::signal(SIGINT, handle_shutdown_signal);
    std::signal(SIGTERM, handle_shutdown_signal);

    std::cout << "dfs_storage: node_id=" << config.node_id << " listening on port " << config.port
               << ", data_dir=" << config.data_dir << "\n";

    while (!g_shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "dfs_storage: shutting down\n";
    node->stop();
    return 0;
}
