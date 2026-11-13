#include "dsm.h"

#include <arpa/inet.h>
#include <unistd.h>

#include <memory>
#include <mutex>
#include <string>

#include "config.h"
#include "directory.h"
#include "log.h"
#include "membership.h"
#include "memory.h"
#include "network.h"
#include "pager.h"

namespace {

// Everything one node owns. Lives from dsm_init to dsm_shutdown; the members are declared
// in the order they must be torn down in reverse (threads first, memory last).
struct Node {
    Config cfg;
    Memory mem;
    Membership membership;
    Network data_net;
    Pager pager;
    Directory directory;
    node_id_t id = NODE_NONE;
    std::mutex alloc_lock;
    uint64_t next_free = 0;  // bytes already handed out of this node's range

    explicit Node(const Config& c)
        : cfg(c), membership(cfg), pager(mem, data_net), directory(mem, data_net) {}
};

std::unique_ptr<Node> g_node;
std::mutex g_lock;

void teardown(Node& n) {
    n.pager.stop();       // restores the previous SIGSEGV action first
    n.data_net.stop();
    n.membership.stop();
    n.mem.shutdown();
}

}  // namespace

int dsm_init(const char* conf_path) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_node) return DSM_EINVAL;
    if (!conf_path) return DSM_ECONFIG;

    Config cfg;
    std::string err;
    int rc = config_load(conf_path, cfg, err);
    if (rc != DSM_OK) {
        fprintf(stderr, "dsm_init: %s\n", err.c_str());
        return rc;
    }
    log_init(cfg.log_dir, cfg.log_level, "pid" + std::to_string(getpid()));

    auto node = std::make_unique<Node>(cfg);
    Node& n = *node;

    rc = n.mem.init(n.cfg);
    if (rc != DSM_OK) return rc;

    rc = n.membership.join();  // picks the slot, so it must precede the data port
    if (rc != DSM_OK) {
        n.membership.stop();
        n.mem.shutdown();
        return rc;
    }
    n.id = n.membership.my_id();

    NodePorts ports = node_ports(n.cfg, n.membership.slot());
    rc = n.data_net.listen(ports.data);
    if (rc != DSM_OK) {
        LOG_ERROR("cannot listen on data port %u", ports.data);
        teardown(n);
        return rc;
    }

    n.mem.activate_home_range(n.id);
    n.directory.init(n.id);

    // Network keeps one handler per message class. Class 1 mixes the requester side (pager)
    // and the home side (directory), so this one dispatches on the type.
    n.data_net.set_handler(msg_class(PAGE_REQUEST),
        [&n](const std::shared_ptr<Connection>& c, const Header& h, const uint8_t* p) {
            if (h.type == PAGE_REQUEST || h.type == FETCH_REPLY || h.type == INVALIDATE_ACK) n.directory.on_message(c, h, p);
            else n.pager.on_coherence_message(c, h, p);
        });
    n.data_net.set_handler(msg_class(KADP_ERROR),
        [&n](const std::shared_ptr<Connection>& c, const Header& h, const uint8_t* p) { n.pager.on_error(c, h, p); });
    rc = n.data_net.start();
    if (rc != DSM_OK) {
        teardown(n);
        return rc;
    }

    n.pager.set_my_node_id(n.id);
    n.pager.set_request_timeout_ms(n.cfg.request_timeout_ms);
    n.pager.set_home_lookup([&n](node_id_t home, std::string& ip, uint16_t& port) {
        NodeInfo info{};
        if (!n.membership.node_info(home, info) || info.state == DEAD || info.state == LEFT) return false;
        in_addr a{};
        a.s_addr = htonl(info.ipv4);
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &a, buf, sizeof buf);
        ip = buf;
        port = info.port_data;
        return true;
    });
    rc = n.pager.start();
    if (rc != DSM_OK) {
        teardown(n);
        return rc;
    }

    LOG_INFO("node %u ready: slot %u, data port %u", n.id, n.membership.slot(), ports.data);
    g_node = std::move(node);
    return DSM_OK;
}

int dsm_shutdown(void) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (!g_node) return DSM_OK;
    teardown(*g_node);
    g_node.reset();
    log_close();
    return DSM_OK;
}

int dsm_node_id(void) { return g_node ? static_cast<int>(g_node->id) : static_cast<int>(DSM_ENOTINIT); }

int dsm_num_nodes(void) {
    if (!g_node) return DSM_ENOTINIT;
    int alive = 0;
    for (const NodeInfo& i : g_node->membership.snapshot())
        if (i.state == ALIVE) alive++;
    return alive;
}

void* dsm_home_base(int node) {
    if (!g_node || node < 0 || static_cast<uint32_t>(node) >= g_node->cfg.max_nodes) return nullptr;
    return g_node->mem.user_addr(g_node->mem.first_page_of(static_cast<node_id_t>(node)));
}

size_t dsm_home_size(void) { return g_node ? static_cast<size_t>(g_node->cfg.node_memory) : 0; }

size_t dsm_space_size(void) {
    return g_node ? static_cast<size_t>(g_node->cfg.max_nodes) * g_node->cfg.node_memory : 0;
}

void* dsm_alloc(size_t size) {
    if (!g_node || size == 0) return nullptr;
    Node& n = *g_node;
    uint64_t ps = n.cfg.page_size;
    uint64_t bytes = (size + ps - 1) / ps * ps;
    std::lock_guard<std::mutex> lk(n.alloc_lock);
    if (n.next_free + bytes > n.cfg.node_memory) return nullptr;
    char* p = static_cast<char*>(dsm_home_base(n.id)) + n.next_free;
    n.next_free += bytes;
    return p;
}

const char* dsm_strerror(int s) {
    switch (s) {
        case DSM_OK: return "ok";
        case DSM_EINVAL: return "invalid argument";
        case DSM_ENOMEM: return "out of memory";
        case DSM_EEXIST: return "already exists";
        case DSM_ENOTFOUND: return "not found";
        case DSM_ENODEDOWN: return "node down";
        case DSM_ETIMEOUT: return "timeout";
        case DSM_ENET: return "network error";
        case DSM_ECONFIG: return "configuration error";
        case DSM_ENOTINIT: return "dsm not initialised";
        case DSM_EFULL: return "cluster full";
        case DSM_EPROTO: return "protocol error";
        case DSM_ENOTALLOC: return "address not allocated";
        default: return "unknown error";
    }
}
