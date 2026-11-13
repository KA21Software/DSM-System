#include "directory.h"

#include <cstring>

#include "log.h"

namespace {
uint64_t bit(node_id_t n) { return 1ULL << n; }
}  // namespace

void Directory::init(node_id_t my_id) {
    my_id_ = my_id;
    slots_.clear();
    slots_.resize(mem_.pages_per_node());
    for (auto& s : slots_) {
        s.e.owner = NODE_NONE;
        s.e.state = D_SHARED;  // untouched memory is zeros held by the home, no copies yet
        s.e.queue_head = s.e.queue_tail = NO_WAITER;
    }
}

Directory::Slot* Directory::slot_of(page_id_t p) {
    if (my_id_ == NODE_NONE || !mem_.valid_page(p) || mem_.home_of(p) != my_id_) return nullptr;
    return &slots_[mem_.offset_of(p)];
}

bool Directory::entry_copy(page_id_t page, DirEntry& out) {
    Slot* s = slot_of(page);
    if (!s) return false;
    std::lock_guard<std::mutex> lk(dir_lock(page));
    out = s->e;
    return true;
}

std::shared_ptr<Connection> Directory::conn_of(node_id_t n) {
    std::lock_guard<std::mutex> lk(conns_lock_);
    if (n < node_conn_.size() && node_conn_[n] && !node_conn_[n]->closed.load()) return node_conn_[n];
    return nullptr;
}

void Directory::on_message(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload) {
    switch (h.type) {
        case PAGE_REQUEST: on_page_request(conn, h); break;
        case INVALIDATE_ACK: on_invalidate_ack(h); break;
        case FETCH_REPLY: on_fetch_reply(h, payload); break;
        default: break;
    }
}

void Directory::send_control(const std::shared_ptr<Connection>& conn, kadp_type type, page_id_t page, version_t version) {
    Header h;
    h.type = type;
    h.page_id = page;
    h.page_version = version;
    h.src_node = my_id_;
    data_net_.send(conn, h, nullptr, 0);
}

void Directory::send_error(const std::shared_ptr<Connection>& conn, const Header& req, int status) {
    Header h;
    h.type = KADP_ERROR;
    h.page_id = req.page_id;  // pager.cpp matches the failed request by page_id
    h.seq = req.seq;
    h.status = static_cast<int16_t>(status);
    h.src_node = my_id_;
    data_net_.send(conn, h, nullptr, 0);
}

void Directory::send_data(page_id_t page, const Req& r, version_t version, bool grant_m, const uint8_t* data) {
    std::vector<uint8_t> buf;
    if (!data) {  // the home's own copy is the current one
        buf.resize(mem_.page_size());
        mem_.copy_out(page, buf.data());
        data = buf.data();
    }
    Header h;
    h.type = PAGE_DATA;
    h.page_id = page;
    h.page_version = version;
    h.seq = r.seq;
    h.src_node = my_id_;
    h.dst_node = r.requester;
    h.flags = F_IS_REPLY | (grant_m ? F_GRANT_M : 0);
    data_net_.send(r.conn, h, data, mem_.page_size());
}

void Directory::on_page_request(const std::shared_ptr<Connection>& conn, const Header& h) {
    page_id_t page = h.page_id;
    Slot* s = slot_of(page);
    if (!s || h.src_node >= 64) {
        LOG_WARN("PAGE_REQUEST for page %u rejected (not homed here or bad node %u)", page, h.src_node);
        send_error(conn, h, DSM_EINVAL);
        return;
    }

    Req req{h.src_node, (h.flags & F_ACCESS_WRITE) ? ACC_WRITE : ACC_READ, h.seq, conn};
    {
        std::lock_guard<std::mutex> lk(conns_lock_);
        if (node_conn_.size() <= req.requester) node_conn_.resize(req.requester + 1);
        node_conn_[req.requester] = conn;
    }

    std::lock_guard<std::mutex> lk(dir_lock(page));
    if (s->e.busy) {
        s->queue.push_back(req);
        LOG_DEBUG("page %u busy, request from node %u queued (%zu waiting)", page, req.requester, s->queue.size());
        return;
    }
    s->cur = req;
    begin(page, *s);
}

// Starts the transition for s.cur. Leaves the page busy only if it must wait for replies.
void Directory::begin(page_id_t page, Slot& s) {
    const Req& r = s.cur;
    s.e.busy = 1;
    LOG_DEBUG("page %u: node %u asks %s (dir state %s, copyset 0x%llx)", page, r.requester,
              r.access == ACC_WRITE ? "write" : "read", s.e.state == D_SHARED ? "SHARED" : "MODIFIED",
              static_cast<unsigned long long>(s.e.copyset));

    if (r.access == ACC_READ) {
        if (s.e.state == D_SHARED) {
            s.e.copyset |= bit(r.requester);
            send_data(page, r, s.e.version, false, nullptr);
            finish(page, s);
            return;
        }
        auto oc = conn_of(s.e.owner);
        if (!oc) {
            LOG_ERROR("page %u: owner %u unreachable", page, s.e.owner);
            Header eh;
            eh.page_id = page;
            eh.seq = r.seq;
            send_error(r.conn, eh, DSM_ENODEDOWN);
            finish(page, s);
            return;
        }
        s.phase = P_WAIT_FETCH;
        send_control(oc, FETCH, page, s.e.version);
        return;
    }

    // Write.
    if (s.e.state == D_MODIFIED) {
        if (s.e.owner == r.requester) {  // it already owns the page; nothing to move
            send_control(r.conn, UPGRADE_ACK, page, s.e.version);
            finish(page, s);
            return;
        }
        auto oc = conn_of(s.e.owner);
        if (!oc) {
            LOG_ERROR("page %u: owner %u unreachable", page, s.e.owner);
            finish(page, s);
            return;
        }
        s.phase = P_WAIT_FETCH;
        send_control(oc, FETCH_INVALIDATE, page, s.e.version);
        return;
    }

    uint64_t others = s.e.copyset & ~bit(r.requester);
    s.acks_left = 0;
    for (node_id_t n = 0; n < 64 && others; n++) {
        if (!(others & bit(n))) continue;
        others &= ~bit(n);
        auto c = conn_of(n);
        if (!c) {  // a copy we can no longer reach cannot be read either; drop it
            s.e.copyset &= ~bit(n);
            continue;
        }
        send_control(c, INVALIDATE, page, s.e.version);
        s.acks_left++;
    }
    if (s.acks_left == 0) {
        grant_write(page, s, nullptr);
        finish(page, s);
    } else {
        s.phase = P_WAIT_INV;
    }
}

// The previous owner/sharers are gone: hand the page to s.cur.requester in M.
void Directory::grant_write(page_id_t page, Slot& s, const uint8_t* data) {
    const Req& r = s.cur;
    bool had_copy = (s.e.copyset & bit(r.requester)) != 0 && s.e.state == D_SHARED;
    s.e.version++;
    s.e.state = D_MODIFIED;
    s.e.owner = r.requester;
    s.e.copyset = bit(r.requester);
    if (had_copy) send_control(r.conn, UPGRADE_ACK, page, s.e.version);  // its S bytes are current
    else send_data(page, r, s.e.version, true, data);
}

void Directory::finish(page_id_t page, Slot& s) {
    for (;;) {
        s.e.busy = 0;
        s.phase = P_IDLE;
        if (s.queue.empty()) return;
        s.cur = std::move(s.queue.front());
        s.queue.pop_front();
        begin(page, s);
        if (s.e.busy) return;  // began a transition that waits for replies
    }
}

void Directory::on_invalidate_ack(const Header& h) {
    page_id_t page = h.page_id;
    Slot* s = slot_of(page);
    if (!s) return;
    std::lock_guard<std::mutex> lk(dir_lock(page));
    if (!s->e.busy || s->phase != P_WAIT_INV || h.src_node >= 64) return;  // stale duplicate
    if (!(s->e.copyset & bit(h.src_node))) return;
    s->e.copyset &= ~bit(h.src_node);
    if (--s->acks_left == 0) {
        grant_write(page, *s, nullptr);
        finish(page, *s);
    }
}

void Directory::on_fetch_reply(const Header& h, const uint8_t* payload) {
    page_id_t page = h.page_id;
    Slot* s = slot_of(page);
    if (!s || h.payload_len != mem_.page_size()) return;
    std::lock_guard<std::mutex> lk(dir_lock(page));
    if (!s->e.busy || s->phase != P_WAIT_FETCH || h.src_node != s->e.owner) return;  // stale

    // The owner stopped writing before it answered, so these bytes are the latest. Store them
    // in the home's copy (system view only) so later readers are served without asking again.
    mem_.store_page(page, payload);

    const Req& r = s->cur;
    node_id_t old_owner = s->e.owner;
    if (r.access == ACC_READ) {
        s->e.state = D_SHARED;
        s->e.copyset = bit(old_owner) | bit(r.requester);  // FETCH leaves the old owner in S
        s->e.owner = NODE_NONE;
        send_data(page, r, s->e.version, false, payload);
    } else {
        s->e.state = D_SHARED;  // so grant_write sends full data, never an UPGRADE_ACK
        s->e.copyset = 0;
        grant_write(page, *s, payload);
    }
    finish(page, *s);
}
