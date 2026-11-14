// Cluster membership (design document, section 7): finding a free slot, joining or founding
// the cluster, the replicated node table, the heartbeat, and (E3) the official end of a node:
// LEAVE for a clean exit, NODE_DEAD broadcast by the coordinator after the heartbeat gave up.
// Every node that learns that a node is gone runs the down handler once, which is how the
// directory, pager, locks and barriers clean up.
//
// Still a deviation from section 7.1: a JOIN that reaches a non-coordinator is rejected, not
// forwarded; the joiner's own probe walks hosts x slots until it reaches the coordinator.
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "config.h"
#include "kadp_structs.h"
#include "kadp_types.h"
#include "network.h"

// Ports this node uses in a given slot (design section 2.4): port = port_base +
// port_stride * slot + offset, offsets 0 data, 1 control, 2 heartbeat, 3 web.
struct NodePorts {
    uint16_t data, ctrl, hb, web;
};
NodePorts node_ports(const Config& cfg, uint8_t slot);

class Membership {
public:
    explicit Membership(const Config& cfg) : cfg_(cfg) {}
    ~Membership() { stop(); }
    Membership(const Membership&) = delete;
    Membership& operator=(const Membership&) = delete;

    // Binds this node's control and heartbeat ports (trying slot 0, 1, 2... as section 7.1
    // describes), then either joins an existing cluster or founds one. On DSM_OK, my_id() and
    // slot() are valid, the node table has at least this node, and the heartbeat is running.
    // DSM_ECONFIG: a reachable coordinator rejected us (different config_hash). DSM_EFULL:
    // the cluster already used every id. DSM_ENET: every port in the slot range is busy.
    int join();
    void stop();

    // Tells the others this node is leaving (LEAVE). The caller then tears the node down.
    void leave();

    node_id_t my_id() const { return my_id_; }
    uint8_t slot() const { return slot_; }
    // Coordinator = lowest-id node that is neither DEAD nor LEFT (section 7.2). A SUSPECT
    // node keeps the role: handing it over on a missed heartbeat would flip it back and forth.
    bool is_coordinator() const;
    node_id_t coordinator_id() const;  // NODE_NONE if there is none

    // The control listener belongs here because JOIN is the first thing it serves; the
    // control plane (allocation, names, queries, locks) registers its classes on it too.
    Network& ctrl_net() { return ctrl_net_; }

    // Wiring done by dsm.cpp before join(); membership does not know the other modules.
    using DownHandler = std::function<void(node_id_t node, bool left)>;
    using SendFn = std::function<int(node_id_t dst, Header h, const void* payload, size_t len)>;
    void set_down_handler(DownHandler fn) { down_handler_ = std::move(fn); }
    void set_send_fn(SendFn fn) { send_fn_ = std::move(fn); }
    // Offered every KADP_ERROR / reply that arrives on the control channel before membership
    // looks at it; true means somebody else's call() consumed it.
    void set_reply_hook(std::function<bool(const Header&, const uint8_t*)> fn) { reply_hook_ = std::move(fn); }
    uint64_t bytes_sent() const { return ctrl_net_.bytes_sent() + hb_net_.bytes_sent(); }
    uint64_t bytes_received() const { return ctrl_net_.bytes_received() + hb_net_.bytes_received(); }
    void set_free_pages_fn(std::function<uint32_t()> fn) { free_pages_fn_ = std::move(fn); }
    // Names table that rides in JOIN_ACK: export fills a payload and returns the entry count;
    // import receives the same bytes at the joiner.
    void set_names_hooks(std::function<uint16_t(std::vector<uint8_t>&)> exp,
                         std::function<void(const uint8_t*, size_t, uint16_t)> imp) {
        names_export_ = std::move(exp);
        names_import_ = std::move(imp);
    }

    // Marks a node DEAD or LEFT (once) and runs the down handler. Used by the heartbeat
    // thread, by NODE_DEAD / LEAVE, and by tests.
    void mark_down(node_id_t id, node_state_t how);

    // For the console/HTML queries (B5) and for modules (pager, directory) that need a
    // peer's address and data-channel port once they are built.
    bool node_info(node_id_t id, NodeInfo& out) const;
    std::vector<NodeInfo> snapshot() const;

private:
    bool bind_slot();
    bool found_cluster();
    int join_existing();  // DSM_OK, DSM_ENODEDOWN (nobody answered: go found), or a rejection
    void send_heartbeats();
    void check_missed_heartbeats();
    void heartbeat_loop();

    void on_ctrl_message(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload);
    void on_ctrl_error(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload);
    void on_heartbeat(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload);
    void handle_join(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload);
    void handle_join_ack(const Header& h, const uint8_t* payload);
    void handle_node_announce(const Header& h, const uint8_t* payload);
    void handle_node_dead(const Header& h, const uint8_t* payload);
    void handle_leave(const Header& h);
    void send_to_others(Header h, const void* payload, size_t len, node_id_t skip);
    void announce_dead(node_id_t id);

    node_id_t add_node_locked(const NodeInfo& info);  // caller holds nodes_lock_ exclusively

    const Config& cfg_;
    uint8_t slot_ = 0;
    uint32_t local_ipv4_ = 0;  // host byte order; set by bind_slot()
    node_id_t my_id_ = NODE_NONE;
    std::atomic<uint32_t> seq_{1};

    Network ctrl_net_, hb_net_;

    mutable std::shared_mutex nodes_lock_;
    std::vector<NodeInfo> nodes_;                       // index == node_id, size == known nodes
    std::vector<std::shared_ptr<Connection>> ctrl_conn_;  // coordinator's link to each joiner
    std::vector<std::shared_ptr<Connection>> hb_conn_;    // per-peer heartbeat link, not replicated

    // Matches the reply to our own outstanding JOIN by seq; a node only ever has one of
    // these in flight (itself), so a single slot is enough (no map needed).
    std::mutex join_wait_lock_;
    std::condition_variable join_wait_cv_;
    bool join_wait_pending_ = false;
    uint32_t join_wait_seq_ = 0;
    int join_wait_status_ = DSM_ETIMEOUT;
    node_id_t join_wait_id_ = NODE_NONE;
    std::vector<NodeInfo> join_wait_table_;
    std::vector<uint8_t> join_wait_names_;
    uint16_t join_wait_n_names_ = 0;

    std::function<bool(const Header&, const uint8_t*)> reply_hook_;
    DownHandler down_handler_;
    SendFn send_fn_;
    std::function<uint32_t()> free_pages_fn_;
    std::function<uint16_t(std::vector<uint8_t>&)> names_export_;
    std::function<void(const uint8_t*, size_t, uint16_t)> names_import_;

    std::thread hb_thread_;
    std::atomic<bool> running_{false};
};
