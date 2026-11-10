// dsm.conf loader and validator (design document, section 8).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Defaults are those of table 8.1; a key missing from the file keeps its default.
struct Config {
    // [cluster]
    std::string cluster_name = "kadp-dsm";
    std::vector<std::string> hosts{"127.0.0.1"};
    uint32_t max_nodes = 8;
    uint32_t max_nodes_per_host = 4;
    // [memory]
    uint32_t page_size = 4096;
    uint64_t node_memory = 1u << 20;
    uint64_t base_address = 0x600000000000ULL;
    // [network]
    uint32_t port_base = 50000;
    uint32_t port_stride = 10;
    uint32_t connect_timeout_ms = 2000;
    uint32_t request_timeout_ms = 3000;
    uint32_t max_retries = 3;
    uint32_t connect_retry_ms = 500;
    // [heartbeat]
    uint32_t hb_interval_ms = 1000;
    uint32_t hb_suspect_after = 3;
    uint32_t hb_dead_after = 5;
    // [cache]
    uint32_t max_cached_pages = 0;
    // [log]
    std::string log_dir = "./logs";
    std::string log_level = "INFO";  // always stored upper-case
    // [web]
    bool web_enabled = true;
    // [extras]
    uint32_t replication_factor = 0;

    uint32_t pages_per_node() const { return static_cast<uint32_t>(node_memory / page_size); }
};

// Reads and validates `path`. Returns DSM_OK, or DSM_ECONFIG with `err` set to a message that
// names the file, the line and the parameter. `out` is only modified on success.
int config_load(const std::string& path, Config& out, std::string& err);

// Fingerprint of the parameters that shape the shared memory (page_size, node_memory,
// max_nodes, base_address). Sent in JOIN; the coordinator rejects a node whose hash differs.
uint32_t config_hash(const Config& c);
