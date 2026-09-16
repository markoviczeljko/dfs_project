#include "process_cluster.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <chrono>
#include <cstring>
#include <fstream>
#include <system_error>
#include <thread>

#include "binary_paths.h"
#include "network/socket.h"

extern char** environ;

namespace dfs_test {

namespace {

// Builds a NUL-terminated argv array backed by `storage` (which must
// outlive the returned vector<char*> — same pattern as every other
// argv-building call site in this file).
std::vector<char*> build_argv(std::vector<std::string>& storage) {
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);
    return argv;
}

}  // namespace

ProcessHandle::~ProcessHandle() {
    if (pid_ <= 0 || reaped_) return;
    ::kill(pid_, SIGKILL);
    int status = 0;
    ::waitpid(pid_, &status, 0);
}

ProcessHandle::ProcessHandle(ProcessHandle&& other) noexcept
    : pid_(other.pid_), reaped_(other.reaped_) {
    other.pid_ = -1;
    other.reaped_ = true;
}

ProcessHandle& ProcessHandle::operator=(ProcessHandle&& other) noexcept {
    if (this != &other) {
        if (pid_ > 0 && !reaped_) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            ::waitpid(pid_, &status, 0);
        }
        pid_ = other.pid_;
        reaped_ = other.reaped_;
        other.pid_ = -1;
        other.reaped_ = true;
    }
    return *this;
}

Result<ProcessHandle> ProcessHandle::spawn(const std::string& executable_path,
                                            const std::vector<std::string>& args) {
    std::vector<std::string> argv_storage;
    argv_storage.reserve(args.size() + 1);
    argv_storage.push_back(executable_path);
    argv_storage.insert(argv_storage.end(), args.begin(), args.end());
    std::vector<char*> argv = build_argv(argv_storage);

    pid_t pid = -1;
    int rc = ::posix_spawn(&pid, executable_path.c_str(), nullptr, nullptr, argv.data(), environ);
    if (rc != 0) {
        return Result<ProcessHandle>::err(ErrorCode::INTERNAL_ERROR);
    }

    ProcessHandle handle;
    handle.pid_ = pid;
    handle.reaped_ = false;
    return Result<ProcessHandle>::ok(std::move(handle));
}

ErrorCode ProcessHandle::send_signal(int sig) const {
    if (pid_ <= 0) {
        return ErrorCode::INVALID_REQUEST;
    }
    if (::kill(pid_, sig) != 0) {
        return ErrorCode::INTERNAL_ERROR;
    }
    return ErrorCode::OK;
}

std::optional<int> ProcessHandle::wait_for_exit(std::chrono::milliseconds timeout) {
    if (pid_ <= 0 || reaped_) {
        return std::nullopt;
    }
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        pid_t result = ::waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
            reaped_ = true;
            return status;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return std::nullopt;
}

bool ProcessHandle::is_running() const {
    if (pid_ <= 0 || reaped_) {
        return false;
    }
    return ::kill(pid_, 0) == 0;
}

bool wait_for_port_ready(const std::string& host, uint16_t port, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto socket_result = Socket::create();
        if (socket_result.is_ok()) {
            Socket socket = std::move(socket_result.value());
            if (socket.connect(host, port) == ErrorCode::OK) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

std::filesystem::path write_master_config(const std::filesystem::path& dir, uint16_t port,
                                           const std::filesystem::path& metadata_dir) {
    std::filesystem::path path = dir / "master.conf";
    std::ofstream out(path);
    out << "port=" << port << "\n";
    out << "metadata_dir=" << metadata_dir.string() << "\n";
    return path;
}

std::filesystem::path write_storage_config(const std::filesystem::path& dir, StorageNodeId node_id,
                                            uint16_t port, const std::filesystem::path& data_dir,
                                            uint16_t master_port, uint64_t capacity_bytes) {
    std::filesystem::path path = dir / ("storage" + std::to_string(node_id) + ".conf");
    std::ofstream out(path);
    out << "node_id=" << node_id << "\n";
    out << "port=" << port << "\n";
    out << "data_dir=" << data_dir.string() << "\n";
    out << "capacity_bytes=" << capacity_bytes << "\n";
    out << "host=127.0.0.1\n";
    out << "master_host=127.0.0.1\n";
    out << "master_port=" << master_port << "\n";
    return path;
}

bool has_any_block_on_disk(const std::filesystem::path& data_dir) {
    std::filesystem::path blocks_dir = data_dir / "blocks";
    std::error_code ec;
    if (!std::filesystem::exists(blocks_dir, ec)) {
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(blocks_dir, ec)) {
        if (entry.path().extension() == ".block") {
            return true;
        }
    }
    return false;
}

Result<ProcessHandle> respawn_storage_node(const std::filesystem::path& config_path) {
    return ProcessHandle::spawn(kDfsStoragePath, {"--config", config_path.string()});
}

int run_client(uint16_t master_port, const std::vector<std::string>& args) {
    std::vector<std::string> full_args = {"--master-host", "127.0.0.1", "--master-port",
                                           std::to_string(master_port)};
    full_args.insert(full_args.end(), args.begin(), args.end());

    auto handle_result = ProcessHandle::spawn(kDfsClientPath, full_args);
    if (handle_result.is_error()) {
        return -1;
    }
    ProcessHandle handle = std::move(handle_result.value());

    auto status = handle.wait_for_exit(std::chrono::seconds(10));
    if (!status.has_value()) {
        return -1;  // handle's destructor SIGKILLs+reaps the stuck process.
    }
    if (WIFEXITED(*status)) {
        return WEXITSTATUS(*status);
    }
    return -1;
}

std::string run_statsctl(uint16_t target_port) {
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
        return {};
    }

    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    ::posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    ::posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);

    std::vector<std::string> argv_storage = {kDfsStatsctlPath, "--host", "127.0.0.1", "--port",
                                              std::to_string(target_port), "stats"};
    std::vector<char*> argv = build_argv(argv_storage);

    pid_t pid = -1;
    int rc = ::posix_spawn(&pid, kDfsStatsctlPath, &actions, nullptr, argv.data(), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(pipe_fds[1]);

    if (rc != 0) {
        ::close(pipe_fds[0]);
        return {};
    }

    std::string output;
    char buffer[4096];
    ssize_t n;
    while ((n = ::read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
        output.append(buffer, static_cast<size_t>(n));
    }
    ::close(pipe_fds[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    return output;
}

void SpawnedCluster::stop_all() {
    master.send_signal(SIGTERM);
    for (ProcessHandle& node : storage_nodes) {
        node.send_signal(SIGTERM);
    }
    master.wait_for_exit(std::chrono::seconds(2));
    for (ProcessHandle& node : storage_nodes) {
        node.wait_for_exit(std::chrono::seconds(2));
    }
}

std::optional<SpawnedCluster> spawn_cluster(const std::filesystem::path& dir, uint16_t master_port,
                                             const std::vector<uint16_t>& storage_ports,
                                             std::chrono::milliseconds ready_timeout) {
    SpawnedCluster cluster;

    auto master_config = write_master_config(dir, master_port, dir / "master_metadata");
    auto master_handle = ProcessHandle::spawn(kDfsMasterPath, {"--config", master_config.string()});
    if (master_handle.is_error()) {
        return std::nullopt;
    }
    cluster.master = std::move(master_handle.value());
    if (!wait_for_port_ready("127.0.0.1", master_port, ready_timeout)) {
        return std::nullopt;
    }

    for (size_t i = 0; i < storage_ports.size(); ++i) {
        auto node_id = static_cast<StorageNodeId>(i + 1);
        std::filesystem::path data_dir = dir / ("node" + std::to_string(node_id));
        auto storage_config = write_storage_config(dir, node_id, storage_ports[i], data_dir, master_port);

        auto storage_handle = ProcessHandle::spawn(kDfsStoragePath, {"--config", storage_config.string()});
        if (storage_handle.is_error()) {
            return std::nullopt;
        }
        cluster.storage_nodes.push_back(std::move(storage_handle.value()));
        cluster.storage_data_dirs.push_back(data_dir);
        cluster.storage_configs.push_back(storage_config);

        if (!wait_for_port_ready("127.0.0.1", storage_ports[i], ready_timeout)) {
            return std::nullopt;
        }
    }

    return cluster;
}

}  // namespace dfs_test
