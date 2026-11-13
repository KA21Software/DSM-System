#include <sys/stat.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "config.h"
#include "kadp_types.h"

static int g_failed = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); g_failed++; } } while (0)

static std::string write_conf(const std::string& body) {
    mkdir("build", 0755);
    mkdir("build/test_conf", 0755);
    std::string path = "build/test_conf/t.conf";
    std::ofstream(path) << body;
    return path;
}

// Loads `body`; returns the status and leaves the message in `err`.
static int load(const std::string& body, Config& c, std::string& err) {
    return config_load(write_conf(body), c, err);
}

static bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

int main() {
    Config c;
    std::string err;

    // The sample file shipped in the repo must load (it has trailing '#' comments).
    CHECK(config_load("conf/dsm.conf", c, err) == DSM_OK);
    CHECK(c.max_nodes == 8 && c.port_base == 28100 && c.node_memory == 1048576);
    CHECK(c.base_address == 0x600000000000ULL && c.pages_per_node() == 256);

    // Empty file -> all defaults.
    Config d;
    CHECK(load("", c, err) == DSM_OK);
    CHECK(c.cluster_name == d.cluster_name && c.hosts.size() == 1 && c.log_level == "INFO");

    // Values: suffixes, hosts list, case-insensitive level, bool.
    CHECK(load("[memory]\nnode_memory = 64M\n[cluster]\nhosts = 10.0.0.11, 10.0.0.12 ,pc-3\n"
               "[log]\nlevel = debug\n[web]\nenabled = false\n", c, err) == DSM_OK);
    CHECK(c.node_memory == 64ULL << 20 && c.hosts.size() == 3 && c.hosts[2] == "pc-3");
    CHECK(c.log_level == "DEBUG" && !c.web_enabled);
    CHECK(load("[memory]\nnode_memory = 128K\n", c, err) == DSM_OK && c.node_memory == 128 * 1024);

    // Errors always return DSM_ECONFIG and name the line and the parameter.
    CHECK(load("[cluster]\nbogus = 1\n", c, err) == DSM_ECONFIG && has(err, ":2:") && has(err, "cluster.bogus"));
    CHECK(load("[nosuch]\nx = 1\n", c, err) == DSM_ECONFIG && has(err, "nosuch.x"));
    CHECK(load("max_nodes = 4\n", c, err) == DSM_ECONFIG && has(err, "outside of a section"));
    CHECK(load("[cluster]\nmax_nodes\n", c, err) == DSM_ECONFIG);
    CHECK(load("[cluster]\nmax_nodes = abc\n", c, err) == DSM_ECONFIG && has(err, "max_nodes"));
    CHECK(load("[cluster]\nmax_nodes = 65\n", c, err) == DSM_ECONFIG && has(err, "out of range"));
    CHECK(load("[cluster]\nmax_nodes = 0\n", c, err) == DSM_ECONFIG);
    CHECK(load("[cluster]\nmax_nodes_per_host = 11\n", c, err) == DSM_ECONFIG);
    CHECK(load("[cluster]\nname = \n", c, err) == DSM_ECONFIG);
    CHECK(load("[cluster]\nhosts = a,,b\n", c, err) == DSM_ECONFIG);
    CHECK(load("[log]\nlevel = LOUD\n", c, err) == DSM_ECONFIG);
    CHECK(load("[web]\nenabled = yes\n", c, err) == DSM_ECONFIG);
    CHECK(load("[memory]\nbase_address = 600000000000\n", c, err) == DSM_ECONFIG && has(err, "hexadecimal"));
    CHECK(load("[memory]\nnode_memory = 32K\n", c, err) == DSM_ECONFIG);  // below 64K
    CHECK(config_load("build/test_conf/missing.conf", c, err) == DSM_ECONFIG && has(err, "cannot open"));

    // Cross rules of section 8.3.
    CHECK(load("[memory]\npage_size = 5000\n", c, err) == DSM_ECONFIG && has(err, "page_size"));
    CHECK(load("[memory]\nnode_memory = 100000\n", c, err) == DSM_ECONFIG && has(err, "node_memory"));
    CHECK(load("[memory]\nbase_address = 0x600000000800\n", c, err) == DSM_ECONFIG && has(err, "aligned"));
    CHECK(load("[memory]\nbase_address = 0x7FFFFFF00000\n", c, err) == DSM_ECONFIG && has(err, "0x7FFFFFFFFFFF"));
    CHECK(load("[network]\nport_base = 65000\nport_stride = 100\n[cluster]\nmax_nodes_per_host = 10\n",
               c, err) == DSM_ECONFIG && has(err, "65535"));
    CHECK(load("[network]\nport_base = 64990\n", c, err) == DSM_OK);  // 64990 + 10*4 still fits
    CHECK(load("[heartbeat]\nsuspect_after = 5\ndead_after = 5\n", c, err) == DSM_ECONFIG && has(err, ":3:"));

    // A failed load must not touch the caller's config.
    Config keep;
    keep.max_nodes = 3;
    CHECK(load("[cluster]\nmax_nodes = 99\n", keep, err) == DSM_ECONFIG && keep.max_nodes == 3);

    // config_hash: stable, and sensitive only to the four memory-shaping parameters.
    Config a, b;
    CHECK(config_hash(a) == config_hash(b));
    b.node_memory = 2u << 20;
    CHECK(config_hash(a) != config_hash(b));
    b = a; b.page_size = 8192;
    CHECK(config_hash(a) != config_hash(b));
    b = a; b.max_nodes = 7;
    CHECK(config_hash(a) != config_hash(b));
    b = a; b.base_address += 4096;
    CHECK(config_hash(a) != config_hash(b));
    b = a; b.port_base = 51000; b.log_level = "DEBUG";
    CHECK(config_hash(a) == config_hash(b));

    if (g_failed) { fprintf(stderr, "%d check(s) failed\n", g_failed); return 1; }
    puts("test_config OK");
    return 0;
}
