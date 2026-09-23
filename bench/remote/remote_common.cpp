#include "remote_common.h"

#include <chrono>
#include <cstdlib>
#include <thread>

#include "bench_common.h"
#include "process_cluster.h"

namespace dfs_bench_remote {

namespace {

// "host:port" or "host:port:vm_name". IPv4 host assumed (no ':' inside the
// host part) — matches this project's whole network layer, which never
// handles IPv6 (see include/network/socket.h).
RemoteNode parse_node(const std::string& arg) {
    size_t first_colon = arg.find(':');
    if (first_colon == std::string::npos) {
        dfs_bench::fail("bad node argument (expected host:port[:vm_name]): " + arg);
    }
    size_t second_colon = arg.find(':', first_colon + 1);

    RemoteNode node;
    node.host = arg.substr(0, first_colon);
    std::string port_str = arg.substr(
        first_colon + 1,
        second_colon == std::string::npos ? std::string::npos : second_colon - first_colon - 1);
    node.port = static_cast<uint16_t>(std::stoul(port_str));
    if (second_colon != std::string::npos) {
        node.vm_name = arg.substr(second_colon + 1);
    }
    return node;
}

}  // namespace

RemoteCluster parse_remote_args(int argc, char** argv) {
    RemoteCluster cluster;
    bool have_master = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--master" && i + 1 < argc) {
            cluster.master = parse_node(argv[++i]);
            have_master = true;
        } else if (arg == "--storage" && i + 1 < argc) {
            cluster.storage.push_back(parse_node(argv[++i]));
        } else if (arg == "--vm-script" && i + 1 < argc) {
            cluster.vm_script = argv[++i];
        } else if (arg == "--rf" && i + 1 < argc) {
            cluster.replication_factor_label = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else {
            dfs_bench::fail("unrecognized or incomplete argument: " + arg);
        }
    }

    if (!have_master) dfs_bench::fail("--master host:port is required");
    if (cluster.storage.empty()) dfs_bench::fail("at least one --storage host:port is required");
    return cluster;
}

void kill_remote_storage(const RemoteCluster& cluster, size_t storage_index) {
    const RemoteNode& node = cluster.storage.at(storage_index);
    if (node.vm_name.empty()) dfs_bench::fail("kill_remote_storage: node has no vm_name");
    if (cluster.vm_script.empty()) dfs_bench::fail("kill_remote_storage: --vm-script not set");

    std::string cmd = "bash " + cluster.vm_script + " kill_storage " + node.vm_name;
    if (std::system(cmd.c_str()) != 0) {
        dfs_bench::fail("kill_remote_storage: script failed: " + cmd);
    }
}

void restart_remote_storage(const RemoteCluster& cluster, size_t storage_index) {
    const RemoteNode& node = cluster.storage.at(storage_index);
    if (node.vm_name.empty()) dfs_bench::fail("restart_remote_storage: node has no vm_name");
    if (cluster.vm_script.empty()) dfs_bench::fail("restart_remote_storage: --vm-script not set");

    std::string cmd = "bash " + cluster.vm_script + " restart_storage " + node.vm_name;
    if (std::system(cmd.c_str()) != 0) {
        dfs_bench::fail("restart_remote_storage: script failed: " + cmd);
    }
    if (!dfs_test::wait_for_port_ready(node.host, node.port, std::chrono::seconds(15))) {
        dfs_bench::fail("restart_remote_storage: node did not come back up: " + node.host + ":" +
                         std::to_string(node.port));
    }
}

void wait_for_storage_quiescent(const RemoteCluster& cluster, std::chrono::milliseconds timeout) {
    auto sum_used_bytes = [&]() {
        uint64_t total = 0;
        for (const auto& node : cluster.storage) {
            std::string stats_text = dfs_test::run_statsctl(node.port, node.host);
            total += dfs_bench::parse_stat_u64(stats_text, "Storage:", "used_bytes=");
        }
        return total;
    };

    auto deadline = std::chrono::steady_clock::now() + timeout;
    uint64_t last = sum_used_bytes();
    int stable_readings = 0;
    // 3 consecutive unchanged readings (~1.5s of no growth) before
    // declaring quiescent — long enough that a still-in-flight COPY_BLOCK
    // between two poll ticks doesn't look stable by coincidence.
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        uint64_t now_value = sum_used_bytes();
        if (now_value == last) {
            if (++stable_readings >= 3) return;
        } else {
            stable_readings = 0;
            last = now_value;
        }
    }
    dfs_bench::fail("wait_for_storage_quiescent: cluster storage usage never stabilized");
}

}  // namespace dfs_bench_remote
