// Distributed locks and barriers (design sections 6.6 and 9.5). The coordinator keeps the
// state in memory: for each lock the holder and a FIFO of waiting nodes, for each barrier the
// nodes that have arrived. A waiting caller sleeps in ControlPlane::call(); the coordinator's
// listener answers later (LOCK_GRANT / BARRIER_RELEASE) from whichever handler frees it, and
// never waits itself.
//
// If a node dies, the coordinator frees its locks (the next waiter gets them) and fails every
// barrier in progress with DSM_ENODEDOWN, because a barrier that counted that node can no
// longer complete. If the coordinator dies, its tables are lost (design section 12.2).
#pragma once
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>

#include "config.h"
#include "control.h"
#include "membership.h"

class Sync {
public:
    Sync(ControlPlane& cp, Membership& ms, const Config& cfg) : cp_(cp), ms_(ms), cfg_(cfg) {}

    int lock(uint32_t lock_id);
    int unlock(uint32_t lock_id);
    int barrier(uint32_t barrier_id, uint16_t n_participants);

    // Coordinator side, on the control listener: LOCK_ACQUIRE, LOCK_RELEASE, BARRIER_ENTER.
    void on_message(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload);
    // A node left or died.
    void on_node_down(node_id_t node);

private:
    struct Waiter {
        node_id_t node;
        std::shared_ptr<Connection> conn;
        Header req;
    };
    struct LockState {
        int holder = -1;
        std::deque<Waiter> queue;
    };
    struct BarrierState {
        uint16_t n = 0;
        std::vector<Waiter> arrived;
    };

    int call_coordinator(kadp_type type, uint32_t id, uint16_t extra, uint32_t timeout_ms);
    void grant_next(uint32_t lock_id, LockState& st);

    ControlPlane& cp_;
    Membership& ms_;
    const Config& cfg_;

    // Client side: threads of one node take a lock one at a time, because the coordinator
    // knows nodes, not threads.
    std::mutex local_lock_;
    std::condition_variable local_cv_;
    std::map<uint32_t, bool> local_held_;

    std::mutex lock_;  // coordinator tables
    std::map<uint32_t, LockState> locks_;
    std::map<uint32_t, BarrierState> barriers_;
};
