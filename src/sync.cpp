#include "sync.h"

#include <unistd.h>

#include "log.h"

namespace {
std::vector<uint8_t> id_payload(uint32_t id, uint16_t extra, bool with_extra) {
    std::vector<uint8_t> p;
    ByteWriter w(p);
    w.u32(id);
    if (with_extra) w.u16(extra);
    return p;
}
}  // namespace

// ------------------------------------------------------------------ client side

// Only LOCK_ACQUIRE is repeated when the coordinator turns out to be gone: nothing was granted
// yet, and the new coordinator starts with empty tables. A barrier that failed with
// DSM_ENODEDOWN was failed on purpose (a participant died) and entering again would count twice.
int Sync::call_coordinator(kadp_type type, uint32_t id, uint16_t extra, uint32_t timeout_ms) {
    const int attempts = type == LOCK_ACQUIRE ? 3 : 1;
    for (int attempt = 0; attempt < attempts; attempt++) {
        node_id_t coord = ms_.coordinator_id();
        if (coord == NODE_NONE) return DSM_ENODEDOWN;
        auto payload = id_payload(id, extra, type == BARRIER_ENTER);
        Header h;
        h.type = type;
        ControlPlane::Reply rep;
        int rc = cp_.call(coord, h, payload.data(), payload.size(), rep, timeout_ms);
        if (rc != DSM_ENODEDOWN) return rc;
        // Not reachable, or not the coordinator any more: give the election a moment.
        usleep(300 * 1000);
        if (ms_.coordinator_id() == coord && attempt > 0) break;
    }
    return DSM_ENODEDOWN;
}

int Sync::lock(uint32_t lock_id) {
    {
        std::unique_lock<std::mutex> lk(local_lock_);
        local_cv_.wait(lk, [&] { return !local_held_[lock_id]; });
        local_held_[lock_id] = true;
    }
    int rc = call_coordinator(LOCK_ACQUIRE, lock_id, 0, 0);
    if (rc != DSM_OK) {
        std::lock_guard<std::mutex> lk(local_lock_);
        local_held_[lock_id] = false;
        local_cv_.notify_all();
    }
    return rc;
}

int Sync::unlock(uint32_t lock_id) {
    {
        std::lock_guard<std::mutex> lk(local_lock_);
        if (!local_held_[lock_id]) return DSM_EINVAL;
    }
    int rc = call_coordinator(LOCK_RELEASE, lock_id, 0, cfg_.request_timeout_ms);
    std::lock_guard<std::mutex> lk(local_lock_);
    local_held_[lock_id] = false;
    local_cv_.notify_all();
    return rc;
}

int Sync::barrier(uint32_t barrier_id, uint16_t n_participants) {
    if (n_participants == 0) return DSM_EINVAL;
    return call_coordinator(BARRIER_ENTER, barrier_id, n_participants, 0);
}

// ------------------------------------------------------------ coordinator side

void Sync::grant_next(uint32_t, LockState& st) {
    while (!st.queue.empty()) {
        Waiter w = st.queue.front();
        st.queue.pop_front();
        NodeInfo info{};
        if (!ms_.node_info(w.node, info) || info.state == DEAD || info.state == LEFT) continue;
        st.holder = w.node;
        cp_.reply(w.conn, w.req, LOCK_GRANT, DSM_OK);
        return;
    }
    st.holder = -1;
}

void Sync::on_message(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload) {
    ByteReader r(payload, h.payload_len);
    uint32_t id = r.u32();
    uint16_t n = h.type == BARRIER_ENTER ? r.u16() : 0;
    if (!r.ok()) return cp_.reply(conn, h, h.type == LOCK_ACQUIRE ? LOCK_GRANT : static_cast<kadp_type>(h.type), DSM_EPROTO);

    if (!ms_.is_coordinator()) {
        return cp_.reply(conn, h, h.type == BARRIER_ENTER ? BARRIER_RELEASE : LOCK_GRANT, DSM_ENODEDOWN);
    }

    std::lock_guard<std::mutex> lk(lock_);
    Waiter w{h.src_node, conn, h};
    switch (h.type) {
        case LOCK_ACQUIRE: {
            LockState& st = locks_[id];
            if (st.holder < 0) {
                st.holder = h.src_node;
                cp_.reply(conn, h, LOCK_GRANT, DSM_OK);
            } else {
                st.queue.push_back(w);  // FIFO; answered by grant_next()
                LOG_DEBUG("lock %u held by node %d, node %u queued", id, st.holder, h.src_node);
            }
            break;
        }
        case LOCK_RELEASE: {
            auto it = locks_.find(id);
            if (it == locks_.end() || it->second.holder != h.src_node) {
                cp_.reply(conn, h, LOCK_RELEASE, DSM_EINVAL);
                break;
            }
            cp_.reply(conn, h, LOCK_RELEASE, DSM_OK);
            grant_next(id, it->second);
            break;
        }
        case BARRIER_ENTER: {
            BarrierState& b = barriers_[id];
            if (n == 0 || (!b.arrived.empty() && b.n != n)) {
                cp_.reply(conn, h, BARRIER_RELEASE, DSM_EINVAL);
                break;
            }
            b.n = n;
            b.arrived.push_back(w);
            if (b.arrived.size() >= b.n) {
                LOG_INFO("barrier %u released (%u participants)", id, b.n);
                for (Waiter& a : b.arrived) cp_.reply(a.conn, a.req, BARRIER_RELEASE, DSM_OK);
                barriers_.erase(id);
            }
            break;
        }
        default:
            break;
    }
}

void Sync::on_node_down(node_id_t node) {
    if (!ms_.is_coordinator()) return;
    std::lock_guard<std::mutex> lk(lock_);
    for (auto& kv : locks_) {
        LockState& st = kv.second;
        for (auto it = st.queue.begin(); it != st.queue.end();)
            it = (it->node == node) ? st.queue.erase(it) : it + 1;
        if (st.holder == static_cast<int>(node)) {
            LOG_WARN("lock %u was held by dead node %u: released", kv.first, node);
            grant_next(kv.first, st);
        }
    }
    for (auto& kv : barriers_)
        for (Waiter& a : kv.second.arrived)
            if (a.node != node) cp_.reply(a.conn, a.req, BARRIER_RELEASE, DSM_ENODEDOWN);
    barriers_.clear();
}
