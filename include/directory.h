// Home-side MSI directory (design document, sections 3.4 to 3.6). Every node is the home of
// one contiguous range of pages and keeps one DirEntry per page of that range. A page goes
// through at most one transition at a time (`busy`); requests that arrive meanwhile wait in a
// per-page FIFO queue.
//
// The home talks to a node over the very connection that node used to send its PAGE_REQUEST,
// never over one it dials itself. A single TCP stream is FIFO, so a PAGE_DATA and a later
// INVALIDATE for the same page can never overtake each other on their way to that node.
//
// Prototype 1 limits (design deviations, see PLAN.md): no timeout for a requester that never
// answers an INVALIDATE/FETCH, and the per-page queue is a std::deque instead of the
// QueuedReq pool of Annex A.
#pragma once
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "kadp_structs.h"
#include "kadp_types.h"
#include "memory.h"
#include "network.h"

class Directory {
public:
    Directory(Memory& mem, Network& data_net) : mem_(mem), data_net_(data_net) {}
    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;

    // Allocates the entries of this node's own range. Call once, when the node ID is known.
    void init(node_id_t my_id);

    // PAGE_REQUEST, FETCH_REPLY and INVALIDATE_ACK. Everything else in class 1 belongs to
    // the Pager; dsm.cpp registers one class-1 handler that dispatches between the two.
    void on_message(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload);

    // For the console queries (E3) and tests: copy of one entry of this node's range.
    bool entry_copy(page_id_t page, DirEntry& out);

private:
    enum Phase : uint8_t { P_IDLE = 0, P_WAIT_INV = 1, P_WAIT_FETCH = 2 };
    struct Req {
        node_id_t requester;
        access_t access;
        uint32_t seq;
        std::shared_ptr<Connection> conn;
    };
    struct Slot {
        DirEntry e{};
        Phase phase = P_IDLE;
        Req cur{};
        uint16_t acks_left = 0;
        std::deque<Req> queue;
    };

    static constexpr uint32_t kStripes = 256;
    std::mutex& dir_lock(page_id_t p) { return locks_[p % kStripes]; }
    Slot* slot_of(page_id_t p);

    void on_page_request(const std::shared_ptr<Connection>& conn, const Header& h);
    void on_invalidate_ack(const Header& h);
    void on_fetch_reply(const Header& h, const uint8_t* payload);

    // The helpers below run with dir_lock(page) held.
    void begin(page_id_t page, Slot& s);
    void finish(page_id_t page, Slot& s);
    void grant_write(page_id_t page, Slot& s, const uint8_t* data);
    void send_data(page_id_t page, const Req& r, version_t version, bool grant_m, const uint8_t* data);
    void send_control(const std::shared_ptr<Connection>& conn, kadp_type type, page_id_t page, version_t version);
    void send_error(const std::shared_ptr<Connection>& conn, const Header& req, int status);
    std::shared_ptr<Connection> conn_of(node_id_t n);

    Memory& mem_;
    Network& data_net_;
    node_id_t my_id_ = NODE_NONE;
    std::vector<Slot> slots_;
    std::mutex locks_[kStripes];

    std::mutex conns_lock_;  // leaf lock: never held while taking another one
    std::vector<std::shared_ptr<Connection>> node_conn_;
};
