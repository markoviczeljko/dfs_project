// Entry point for the dfs_client executable (see docs/design.md §11).
// Usage:
//   dfs_client --master-host <host> --master-port <port> create <path>
//   dfs_client --master-host <host> --master-port <port> write <path> <local_source_file>
//   dfs_client --master-host <host> --master-port <port> read <path> <local_dest_file>
//   dfs_client --master-host <host> --master-port <port> append <path> <local_source_file>
//   dfs_client --master-host <host> --master-port <port> delete <path>
//   dfs_client --master-host <host> --master-port <port> list <path>
//   dfs_client --master-host <host> --master-port <port> stat <path>

#include <exception>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "client/client.h"

namespace {

void print_usage(const char* argv0) {
    std::cerr << "Usage:\n"
              << "  " << argv0 << " --master-host <host> --master-port <port> create <path>\n"
              << "  " << argv0
              << " --master-host <host> --master-port <port> write <path> <local_source_file>\n"
              << "  " << argv0
              << " --master-host <host> --master-port <port> read <path> <local_dest_file>\n"
              << "  " << argv0
              << " --master-host <host> --master-port <port> append <path> <local_source_file>\n"
              << "  " << argv0 << " --master-host <host> --master-port <port> delete <path>\n"
              << "  " << argv0 << " --master-host <host> --master-port <port> list <path>\n"
              << "  " << argv0 << " --master-host <host> --master-port <port> stat <path>\n";
}

Result<std::vector<uint8_t>> read_local_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return Result<std::vector<uint8_t>>::err(ErrorCode::INVALID_REQUEST);
    }
    return Result<std::vector<uint8_t>>::ok(
        std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()));
}

ErrorCode write_local_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return ErrorCode::INTERNAL_ERROR;
    }
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return file.good() ? ErrorCode::OK : ErrorCode::INTERNAL_ERROR;
}

}  // namespace

int main(int argc, char** argv) {
    std::string master_host;
    uint16_t master_port = 0;
    std::vector<std::string> positional;

    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--master-host" && i + 1 < argc) {
                master_host = argv[++i];
            } else if (arg == "--master-port" && i + 1 < argc) {
                unsigned long parsed = std::stoul(argv[++i]);
                if (parsed > 65535) {
                    print_usage(argv[0]);
                    return 1;
                }
                master_port = static_cast<uint16_t>(parsed);
            } else {
                positional.push_back(arg);
            }
        }
    } catch (const std::exception&) {
        print_usage(argv[0]);
        return 1;
    }

    if (master_host.empty() || master_port == 0 || positional.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    auto client_result = Client::create(Client::Config{master_host, master_port});
    if (client_result.is_error()) {
        std::cerr << "Failed to connect to Master at " << master_host << ":" << master_port << "\n";
        return 1;
    }
    auto client = std::move(client_result.value());

    const std::string& command = positional[0];

    if (command == "create" && positional.size() == 2) {
        ErrorCode result = client->create_file(positional[1]);
        if (result != ErrorCode::OK) {
            std::cerr << "create failed: error_code=" << static_cast<int>(result) << "\n";
            return 1;
        }
        std::cout << "created " << positional[1] << "\n";
        return 0;
    }

    if (command == "write" && positional.size() == 3) {
        auto data_result = read_local_file(positional[2]);
        if (data_result.is_error()) {
            std::cerr << "Failed to read local file: " << positional[2] << "\n";
            return 1;
        }
        ErrorCode result = client->write(positional[1], data_result.value());
        if (result != ErrorCode::OK) {
            std::cerr << "write failed: error_code=" << static_cast<int>(result) << "\n";
            return 1;
        }
        std::cout << "wrote " << data_result.value().size() << " bytes to " << positional[1] << "\n";
        return 0;
    }

    if (command == "read" && positional.size() == 3) {
        auto data_result = client->read(positional[1]);
        if (data_result.is_error()) {
            std::cerr << "read failed: error_code=" << static_cast<int>(data_result.error()) << "\n";
            return 1;
        }
        if (write_local_file(positional[2], data_result.value()) != ErrorCode::OK) {
            std::cerr << "Failed to write local file: " << positional[2] << "\n";
            return 1;
        }
        std::cout << "read " << data_result.value().size() << " bytes from " << positional[1] << "\n";
        return 0;
    }

    if (command == "append" && positional.size() == 3) {
        auto data_result = read_local_file(positional[2]);
        if (data_result.is_error()) {
            std::cerr << "Failed to read local file: " << positional[2] << "\n";
            return 1;
        }
        ErrorCode result = client->append(positional[1], data_result.value());
        if (result != ErrorCode::OK) {
            std::cerr << "append failed: error_code=" << static_cast<int>(result) << "\n";
            return 1;
        }
        std::cout << "appended " << data_result.value().size() << " bytes to " << positional[1] << "\n";
        return 0;
    }

    if (command == "delete" && positional.size() == 2) {
        ErrorCode result = client->delete_file(positional[1]);
        if (result != ErrorCode::OK) {
            std::cerr << "delete failed: error_code=" << static_cast<int>(result) << "\n";
            return 1;
        }
        std::cout << "deleted " << positional[1] << "\n";
        return 0;
    }

    if (command == "list" && positional.size() == 2) {
        auto list_result = client->list(positional[1]);
        if (list_result.is_error()) {
            std::cerr << "list failed: error_code=" << static_cast<int>(list_result.error()) << "\n";
            return 1;
        }
        for (const std::string& entry : list_result.value()) {
            std::cout << entry << "\n";
        }
        return 0;
    }

    if (command == "stat" && positional.size() == 2) {
        auto stat_result = client->stat(positional[1]);
        if (stat_result.is_error()) {
            std::cerr << "stat failed: error_code=" << static_cast<int>(stat_result.error()) << "\n";
            return 1;
        }
        const Client::FileStat& stat = stat_result.value();
        std::cout << "id=" << stat.id << " size=" << stat.size
                   << " modification_time=" << stat.modification_time
                   << " blocks=" << stat.blocks.size() << "\n";
        for (BlockId block_id : stat.blocks) {
            std::cout << "  block " << block_id << "\n";
        }
        return 0;
    }

    print_usage(argv[0]);
    return 1;
}
