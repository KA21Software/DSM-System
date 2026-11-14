// Request/response layer over the control channel (class 0x2_ allocation and names, 0x4_
// queries, 0x5_ locks and barriers). A caller sends a request to a node and sleeps until the
// matching reply (same seq, F_IS_REPLY) comes back through the listener, or until the target
// is declared down. Handlers on the listener thread never call call(): they only answer, or
// park the request and answer later (design section 4.4: the listener never waits).
//
// Requests to the node's own control port go through the network too, like the pager does
// with its own data port: one path for local and remote targets.
#pragma once
#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "membership.h"
#include "network.h"

class ControlPlane {
public:
    struct Reply {
        Header h;
        std::vector<uint8_t> payload;
    };

    ControlPlane(Membership& membership, const Config& cfg) : ms_(membership), cfg_(cfg) {}

    // Connection to a node's control port, opened on first use and reused. nullptr if the
    // node is unknown, DEAD or LEFT, or if it cannot be reached.
    std::shared_ptr<Connection> connection_to(node_id_t node);

    // Sends `h` (src/dst/seq are filled in) and waits for the reply. Returns the reply's
    // status (0 or a DSM_E* code), or DSM_ENODEDOWN if the target is not reachable / dies
    // while waiting, or DSM_ETIMEOUT. timeout_ms == 0 waits without limit (locks, barriers).
    int call(node_id_t dst, Header h, const void* payload, size_t len, Reply& out, uint32_t timeout_ms);
    int send(node_id_t dst, Header h, const void* payload, size_t len);
    // To every node that is not DEAD or LEFT, optionally including this one.
    void broadcast(Header h, const void* payload, size_t len, bool include_self);

    // Listener side: answers a request on the connection it arrived on.
    void reply(const std::shared_ptr<Connection>& conn, const Header& req, kadp_type type, int status,
               const void* payload = nullptr, size_t len = 0);

    // Called for every message with F_IS_REPLY and for KADP_ERROR. False if no call() waits for it.
    bool on_reply(const Header& h, const uint8_t* payload);
    // A node went down: its pending calls end with DSM_ENODEDOWN.
    void fail_pending_for(node_id_t node);

    Network& net() { return ms_.ctrl_net(); }

private:
    struct Pending {
        node_id_t dst;
        bool done = false;
        int status = 0;
        Header h;
        std::vector<uint8_t> payload;
    };

    Membership& ms_;
    const Config& cfg_;
    std::mutex conn_lock_;  // leaf lock
    std::vector<std::shared_ptr<Connection>> conn_;
    std::atomic<uint32_t> seq_{1000000};  // apart from membership's seq range, easier to read in logs

    std::mutex pend_lock_;
    std::condition_variable pend_cv_;
    std::unordered_map<uint32_t, Pending> pending_;
};
