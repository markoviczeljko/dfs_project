// Entry point for the dfs_statsctl executable (see docs/design.md §18,
// design notes §10.33). Works identically against a live dfs_master or
// dfs_storage process — both implement GET_STATS the same way.
// Usage:
//   dfs_statsctl --host <host> --port <port> stats

#include <exception>
#include <iostream>
#include <string>

#include "network/connection.h"
#include "network/socket.h"

namespace {

void print_usage(const char* argv0) {
    std::cerr << "Usage:\n  " << argv0 << " --host <host> --port <port> stats\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string host;
    uint16_t port = 0;
    std::string command;

    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--host" && i + 1 < argc) {
                host = argv[++i];
            } else if (arg == "--port" && i + 1 < argc) {
                unsigned long parsed = std::stoul(argv[++i]);
                if (parsed > 65535) {
                    print_usage(argv[0]);
                    return 1;
                }
                port = static_cast<uint16_t>(parsed);
            } else {
                command = arg;
            }
        }
    } catch (const std::exception&) {
        print_usage(argv[0]);
        return 1;
    }

    if (host.empty() || port == 0 || command != "stats") {
        print_usage(argv[0]);
        return 1;
    }

    auto socket_result = Socket::create();
    if (socket_result.is_error()) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }
    Socket socket = std::move(socket_result.value());
    if (socket.connect(host, port) != ErrorCode::OK) {
        std::cerr << "Failed to connect to " << host << ":" << port << "\n";
        return 1;
    }
    TCPConnection connection(std::move(socket));

    if (!connection.sendMessage(Message{MessageType::GET_STATS, 1, {}})) {
        std::cerr << "Failed to send GET_STATS\n";
        return 1;
    }
    auto response = connection.receiveMessage();
    if (!response.has_value() || response->type != MessageType::STATS_RESPONSE) {
        std::cerr << "Did not receive a valid STATS_RESPONSE\n";
        return 1;
    }

    std::cout.write(reinterpret_cast<const char*>(response->payload.data()),
                     static_cast<std::streamsize>(response->payload.size()));
    return 0;
}
