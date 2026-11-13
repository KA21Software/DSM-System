// Cluster membership (design document, section 7): finding a free slot, joining or founding
// the cluster, the replicated node table, and the heartbeat. This is the E2 "minimal" version
// (PLAN.md, Fase 1): SUSPECT is detected locally by every node, as section 7.3 already
// describes; officialising DEAD through the coordinator (NODE_DEAD, page cleanup, coordinator
// hand-off) and JOIN forwarding across more than one hop are Fase 2 (E3) work.
#pragma once
#include <atomic>
#include <condition_variable>
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

    node_id_t my_id() const { return my_id_; }
    uint8_t slot() const { return slot_; }
    bool is_coordinator() const;  // the lowest-id ALIVE node, per section 7.2

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

    std::thread hb_thread_;
    std::atomic<bool> running_{false};
};
