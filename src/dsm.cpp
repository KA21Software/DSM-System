#include "dsm.h"

#include <arpa/inet.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "alloc.h"
#include "config.h"
#include "control.h"
#include "directory.h"
#include "log.h"
#include "membership.h"
#include "memory.h"
#include "names.h"
#include "network.h"
#include "pager.h"
#include "query.h"
#include "sync.h"
#include "web.h"

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
    ControlPlane cp;
    Allocator alloc;
    Names names;
    Sync sync;
    Query query;
    Web web;
    node_id_t id = NODE_NONE;
    std::thread maint;
    std::atomic<bool> maint_running{false};

    explicit Node(const Config& c)
        : cfg(c),
          membership(cfg),
          pager(mem, data_net),
          directory(mem, data_net),
          cp(membership, cfg),
          alloc(mem, directory, cp, membership, cfg),
          names(cp, membership, cfg),
          sync(cp, membership, cfg),
          query(cfg, mem, membership, data_net, directory, alloc, names, cp),
          web(query) {}
};

std::unique_ptr<Node> g_node;
std::mutex g_lock;
thread_local int t_last_error = DSM_OK;

int fail(int code) {
    t_last_error = code;
    return code;
}

void* fail_ptr(int code) {
    t_last_error = code;
    return nullptr;
}

void teardown(Node& n) {
    n.web.stop();
    n.query.stop_console();
    n.maint_running = false;
    if (n.maint.joinable()) n.maint.join();
    n.pager.stop();  // restores the previous SIGSEGV action first
    n.data_net.stop();
    n.membership.stop();
    n.mem.shutdown();
}

// Periodic work that has no thread of its own: give up directory transitions whose node
// stopped answering. 250 ms keeps the delay well under request_timeout_ms.
void maintenance_loop(Node* n) {
    uint32_t limit = n->cfg.request_timeout_ms * 3 / 4;
    while (n->maint_running.load()) {
        n->directory.sweep(limit);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

bool console_wanted() {
    const char* e = getenv("DSM_CONSOLE");
    if (e) return e[0] == '1';
    return isatty(STDIN_FILENO);
}

// Class handlers of the control channel (design table 6.2). A reply goes to whoever is
// waiting in ControlPlane::call(); a request goes to the module that serves it.
void wire_control(Node& n) {
    Network& net = n.membership.ctrl_net();
    net.set_handler(0x2, [&n](const std::shared_ptr<Connection>& c, const Header& h, const uint8_t* p) {
        if (h.flags & F_IS_REPLY) {
            n.cp.on_reply(h, p);
        } else if (h.type == ALLOC_REQUEST || h.type == FREE) {
            n.alloc.on_message(c, h, p);
        } else {
            n.names.on_message(c, h, p);
        }
    });
    net.set_handler(0x4, [&n](const std::shared_ptr<Connection>& c, const Header& h, const uint8_t* p) {
        if (h.flags & F_IS_REPLY) n.cp.on_reply(h, p);
        else n.query.on_message(c, h, p);
    });
    net.set_handler(0x5, [&n](const std::shared_ptr<Connection>& c, const Header& h, const uint8_t* p) {
        if (h.flags & F_IS_REPLY) n.cp.on_reply(h, p);
        else n.sync.on_message(c, h, p);
    });
    n.membership.set_reply_hook([&n](const Header& h, const uint8_t* p) { return n.cp.on_reply(h, p); });
    n.membership.set_send_fn([&n](node_id_t dst, Header h, const void* p, size_t len) { return n.cp.send(dst, h, p, len); });
    n.membership.set_free_pages_fn([&n] { return n.alloc.free_pages(); });
    n.membership.set_names_hooks([&n](std::vector<uint8_t>& out) { return n.names.export_all(out); },
                                 [&n](const uint8_t* d, size_t len, uint16_t count) { n.names.import_all(d, len, count); });
    n.membership.set_down_handler([&n](node_id_t node, bool left) {
        LOG_INFO("node %u %s: cleaning up", node, left ? "left" : "died");
        // A node that left cleanly may still have replies in flight (the final barrier's
        // release, for one): call() notices on its own, after a grace period, if none comes.
        if (!left) n.cp.fail_pending_for(node);
        n.pager.on_node_down(node);
        n.directory.on_node_down(node);
        n.sync.on_node_down(node);
    });
}

}  // namespace

int dsm_init(const char* conf_path) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_node) return fail(DSM_EINVAL);
    if (!conf_path) return fail(DSM_ECONFIG);

    Config cfg;
    std::string err;
    int rc = config_load(conf_path, cfg, err);
    if (rc != DSM_OK) {
        fprintf(stderr, "dsm_init: %s\n", err.c_str());
        return fail(rc);
    }
    log_init(cfg.log_dir, cfg.log_level, "pid" + std::to_string(getpid()));

    auto node = std::make_unique<Node>(cfg);
    Node& n = *node;

    rc = n.mem.init(n.cfg);
    if (rc != DSM_OK) return fail(rc);

    wire_control(n);
    rc = n.membership.join();  // picks the slot, so it must precede the data port
    if (rc != DSM_OK) {
        n.membership.stop();
        n.mem.shutdown();
        return fail(rc);
    }
    n.id = n.membership.my_id();

    NodePorts ports = node_ports(n.cfg, n.membership.slot());
    rc = n.data_net.listen(ports.data);
    if (rc != DSM_OK) {
        LOG_ERROR("cannot listen on data port %u", ports.data);
        teardown(n);
        return fail(rc);
    }

    n.mem.activate_home_range(n.id);
    n.directory.init(n.id);
    n.alloc.init(n.id);

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
        return fail(rc);
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
        return fail(rc);
    }

    n.maint_running = true;
    n.maint = std::thread(maintenance_loop, &n);
    if (n.cfg.web_enabled) n.web.start(ports.web);
    if (console_wanted()) {
        n.query.start_console([] {
            dsm_shutdown();
            fflush(stdout);
            _exit(0);  // the application's own threads may still be running
        });
    }

    LOG_INFO("node %u ready: slot %u, data port %u", n.id, n.membership.slot(), ports.data);
    g_node = std::move(node);
    return DSM_OK;
}

int dsm_shutdown(void) {
    std::unique_ptr<Node> node;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (!g_node) return DSM_OK;
        node = std::move(g_node);
    }
    node->membership.leave();
    teardown(*node);
    node.reset();
    log_close();
    return DSM_OK;
}

int dsm_node_id(void) { return g_node ? static_cast<int>(g_node->id) : DSM_ENOTINIT; }

int dsm_num_nodes(void) {
    if (!g_node) return DSM_ENOTINIT;
    int alive = 0;
    for (const NodeInfo& i : g_node->membership.snapshot())
        if (i.state == ALIVE || i.state == SUSPECT) alive++;
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

static void* alloc_impl(size_t size, const std::string& name) {
    if (!g_node) return fail_ptr(DSM_ENOTINIT);
    if (size == 0) return fail_ptr(DSM_EINVAL);
    int64_t page = g_node->alloc.allocate(size, name);
    if (page < 0) return fail_ptr(static_cast<int>(page));
    return g_node->mem.user_addr(static_cast<page_id_t>(page));
}

void* dsm_alloc(size_t size) { return alloc_impl(size, ""); }

void* dsm_alloc_named(const char* name, size_t size) {
    if (!g_node) return fail_ptr(DSM_ENOTINIT);
    if (!name || !*name || strlen(name) > 31 || size == 0) return fail_ptr(DSM_EINVAL);
    Node& n = *g_node;

    NameEntry found;
    if (n.names.lookup(name, found) == DSM_OK) {
        if (found.size_bytes != size) return fail_ptr(DSM_EEXIST);
        return n.mem.user_addr(found.first_page);
    }

    // Not there yet. Allocate first, register second: if another node registered the same
    // name in between, the coordinator says so and this block is given back.
    uint32_t alloc_id = 0;
    int64_t page = n.alloc.allocate(size, name, &alloc_id);
    if (page < 0) return fail_ptr(static_cast<int>(page));

    NameEntry e{};
    strncpy(e.name, name, sizeof e.name - 1);
    e.alloc_id = alloc_id;
    e.first_page = static_cast<uint32_t>(page);
    e.size_bytes = size;
    e.owner_node = n.id;
    int rc = n.names.register_name(e);
    if (rc == DSM_OK) return n.mem.user_addr(static_cast<page_id_t>(page));

    uint32_t freed_id;
    n.alloc.release(static_cast<page_id_t>(page), freed_id);  // lost the race (or no coordinator)
    if (rc == DSM_EEXIST && e.size_bytes == size) return n.mem.user_addr(e.first_page);
    return fail_ptr(rc);
}

void* dsm_lookup(const char* name, size_t* size_out) {
    if (!g_node) return fail_ptr(DSM_ENOTINIT);
    if (!name || !*name) return fail_ptr(DSM_EINVAL);
    NameEntry e;
    int rc = g_node->names.lookup(name, e);
    if (rc != DSM_OK) return fail_ptr(rc);
    if (size_out) *size_out = static_cast<size_t>(e.size_bytes);
    return g_node->mem.user_addr(e.first_page);
}

int dsm_free(void* ptr) {
    if (!g_node) return fail(DSM_ENOTINIT);
    Node& n = *g_node;
    if (!ptr || !n.mem.contains(ptr)) return fail(DSM_EINVAL);
    page_id_t page = n.mem.page_of(ptr);
    if (n.mem.user_addr(page) != ptr) return fail(DSM_EINVAL);
    uint32_t alloc_id = 0;
    int rc = n.alloc.release(page, alloc_id);
    if (rc == DSM_OK || rc == DSM_ENODEDOWN) n.names.remove_by_alloc(alloc_id);
    return rc == DSM_OK ? DSM_OK : fail(rc);
}

int dsm_lock(uint32_t id) {
    if (!g_node) return fail(DSM_ENOTINIT);
    int rc = g_node->sync.lock(id);
    return rc == DSM_OK ? rc : fail(rc);
}

int dsm_unlock(uint32_t id) {
    if (!g_node) return fail(DSM_ENOTINIT);
    int rc = g_node->sync.unlock(id);
    return rc == DSM_OK ? rc : fail(rc);
}

int dsm_barrier(uint32_t id, uint16_t n) {
    if (!g_node) return fail(DSM_ENOTINIT);
    int rc = g_node->sync.barrier(id, n);
    return rc == DSM_OK ? rc : fail(rc);
}

int dsm_get_stats(dsm_stats_t* out) {
    if (!g_node) return fail(DSM_ENOTINIT);
    if (!out) return fail(DSM_EINVAL);
    g_node->query.local_stats(*out);
    return DSM_OK;
}

int dsm_get_node_stats(uint16_t node_id, dsm_stats_t* out) {
    if (!g_node) return fail(DSM_ENOTINIT);
    if (!out) return fail(DSM_EINVAL);
    int rc = g_node->query.node_stats(node_id, *out);
    return rc == DSM_OK ? rc : fail(rc);
}

int dsm_get_global_stats(dsm_stats_t* out) {
    if (!g_node) return fail(DSM_ENOTINIT);
    if (!out) return fail(DSM_EINVAL);
    return g_node->query.global_stats(*out);
}

int dsm_last_error(void) { return t_last_error; }

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
