// Home-side MSI directory (design document, sections 3.4 to 3.6). Every node is the home of
// one contiguous range of pages and keeps one DirEntry per page of that range. A page goes
// through at most one transition at a time (`busy`); requests that arrive meanwhile wait in a
// per-page FIFO queue.
//
// The home talks to a node over the very connection that node used to send its PAGE_REQUEST,
// never over one it dials itself. A single TCP stream is FIFO, so a PAGE_DATA and a later
// INVALIDATE for the same page can never overtake each other on their way to that node.
//
// Deviations from the design (see PLAN.md): the per-page queue is a std::deque instead of the
// QueuedReq pool of Annex A. E3 additions: a page must be allocated to be served
// (DSM_ENOTALLOC), freeing a page is one more transition in the same queue, a transition that
// waits too long for a node is abandoned with DSM_ETIMEOUT, and a dead node is removed from
// every copyset (its M pages become LOST).
#pragma once
#include <deque>
#include <functional>
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

    // The allocator tells the directory which pages of this range are in use. Requests for a
    // page that is not allocated are answered with DSM_ENOTALLOC.
    void set_allocated(page_id_t first, uint32_t n_pages, bool allocated);
    // Frees one page: invalidates every copy, zeroes the home's bytes and resets the entry.
    // Queued behind whatever transition is running. `done` runs once the page is clean, with
    // the page lock held, so it must only touch atomics and send messages.
    void free_page(page_id_t page, std::function<void()> done);

    // A node left or died: forget its copies, finish transitions that waited for it, mark the
    // pages only it held as LOST.
    void on_node_down(node_id_t node);
    // Gives up transitions that have waited longer than `timeout_ms` for a node.
    void sweep(uint32_t timeout_ms);
    // For the console: the entry plus the flags that live next to it.
    bool entry_info(page_id_t page, DirEntry& out, bool& allocated, bool& lost);

private:
    enum Phase : uint8_t { P_IDLE = 0, P_WAIT_INV = 1, P_WAIT_FETCH = 2 };
    struct Req {
        node_id_t requester;
        access_t access;
        uint32_t seq;
        std::shared_ptr<Connection> conn;
        std::function<void()> on_free;  // set only for the internal "free this page" request
    };
    struct Slot {
        DirEntry e{};
        Phase phase = P_IDLE;
        Req cur{};
        uint16_t acks_left = 0;
        uint64_t started_ms = 0;
        bool allocated = false;
        bool lost = false;
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
    void complete_free(page_id_t page, Slot& s);
    void fail_current(page_id_t page, Slot& s, int status);
    void send_data(page_id_t page, const Req& r, version_t version, bool grant_m, const uint8_t* data);
    void send_control(const std::shared_ptr<Connection>& conn, kadp_type type, page_id_t page, version_t version);
    void send_error(const std::shared_ptr<Connection>& conn, const Header& req, int status);
    std::shared_ptr<Connection> conn_of(node_id_t n);
    bool is_down(node_id_t n);

    Memory& mem_;
    Network& data_net_;
    node_id_t my_id_ = NODE_NONE;
    std::vector<Slot> slots_;
    std::mutex locks_[kStripes];

    std::mutex conns_lock_;  // leaf lock: never held while taking another one
    std::vector<std::shared_ptr<Connection>> node_conn_;
    std::vector<bool> down_;  // guarded by conns_lock_
};
