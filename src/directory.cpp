#include "directory.h"

#include <chrono>
#include <cstring>

#include "log.h"
#include "stats.h"

namespace {
uint64_t bit(node_id_t n) { return n < 64 ? 1ULL << n : 0; }
uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
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
    bool a, l;
    return entry_info(page, out, a, l);
}

bool Directory::entry_info(page_id_t page, DirEntry& out, bool& allocated, bool& lost) {
    Slot* s = slot_of(page);
    if (!s) return false;
    std::lock_guard<std::mutex> lk(dir_lock(page));
    out = s->e;
    allocated = s->allocated;
    lost = s->lost;
    return true;
}

std::shared_ptr<Connection> Directory::conn_of(node_id_t n) {
    std::lock_guard<std::mutex> lk(conns_lock_);
    if (n < node_conn_.size() && node_conn_[n] && !node_conn_[n]->closed.load()) return node_conn_[n];
    return nullptr;
}

bool Directory::is_down(node_id_t n) {
    std::lock_guard<std::mutex> lk(conns_lock_);
    return n < down_.size() && down_[n];
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
    if (type == INVALIDATE) counters().add(counters().invalidations_sent);
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
    counters().add(counters().pages_sent);
}

void Directory::set_allocated(page_id_t first, uint32_t n_pages, bool allocated) {
    for (page_id_t p = first; p < first + n_pages; p++) {
        Slot* s = slot_of(p);
        if (!s) continue;
        std::lock_guard<std::mutex> lk(dir_lock(p));
        s->allocated = allocated;
    }
}

void Directory::free_page(page_id_t page, std::function<void()> done) {
    Slot* s = slot_of(page);
    if (!s) {
        done();
        return;
    }
    Req req{NODE_NONE, ACC_WRITE, 0, nullptr, std::move(done)};
    std::lock_guard<std::mutex> lk(dir_lock(page));
    s->allocated = false;  // from here on, new requests get DSM_ENOTALLOC
    if (s->e.busy) {
        s->queue.push_back(std::move(req));
        return;
    }
    s->cur = std::move(req);
    begin(page, *s);
}

void Directory::on_page_request(const std::shared_ptr<Connection>& conn, const Header& h) {
    page_id_t page = h.page_id;
    Slot* s = slot_of(page);
    if (!s || h.src_node >= 64) {
        LOG_WARN("PAGE_REQUEST for page %u rejected (not homed here or bad node %u)", page, h.src_node);
        send_error(conn, h, DSM_EINVAL);
        return;
    }

    Req req{h.src_node, (h.flags & F_ACCESS_WRITE) ? ACC_WRITE : ACC_READ, h.seq, conn, nullptr};
    {
        std::lock_guard<std::mutex> lk(conns_lock_);
        if (node_conn_.size() <= req.requester) node_conn_.resize(req.requester + 1);
        node_conn_[req.requester] = conn;
    }

    std::lock_guard<std::mutex> lk(dir_lock(page));
    if (!s->allocated) {
        send_error(conn, h, DSM_ENOTALLOC);
        return;
    }
    if (s->lost) {
        send_error(conn, h, DSM_ENODEDOWN);
        return;
    }
    if (s->e.busy) {
        s->queue.push_back(req);
        LOG_DEBUG("page %u busy, request from node %u queued (%zu waiting)", page, req.requester, s->queue.size());
        return;
    }
    s->cur = req;
    begin(page, *s);
}

// A request that cannot be served: answer it with `status` and move on to the next one.
void Directory::fail_current(page_id_t page, Slot& s, int status) {
    if (s.cur.on_free) {
        complete_free(page, s);
    } else if (s.cur.conn) {
        Header eh;
        eh.page_id = page;
        eh.seq = s.cur.seq;
        send_error(s.cur.conn, eh, status);
    }
    finish(page, s);
}

// Starts the transition for s.cur. Leaves the page busy only if it must wait for replies.
void Directory::begin(page_id_t page, Slot& s) {
    const Req& r = s.cur;
    s.e.busy = 1;
    s.started_ms = now_ms();

    if (!r.on_free) {
        if (!s.allocated) return fail_current(page, s, DSM_ENOTALLOC);
        if (s.lost) return fail_current(page, s, DSM_ENODEDOWN);
        if (is_down(r.requester)) return finish(page, s);  // nobody left to answer
    }
    LOG_DEBUG("page %u: node %u asks %s (dir state %s, copyset 0x%llx)", page, r.requester,
              r.on_free ? "free" : r.access == ACC_WRITE ? "write" : "read",
              s.e.state == D_SHARED ? "SHARED" : "MODIFIED", static_cast<unsigned long long>(s.e.copyset));

    if (r.access == ACC_READ && !r.on_free) {
        if (s.e.state == D_SHARED) {
            s.e.copyset |= bit(r.requester);
            send_data(page, r, s.e.version, false, nullptr);
            finish(page, s);
            return;
        }
        auto oc = conn_of(s.e.owner);
        if (!oc) {
            LOG_ERROR("page %u: owner %u unreachable, page is LOST", page, s.e.owner);
            s.lost = true;
            return fail_current(page, s, DSM_ENODEDOWN);
        }
        s.phase = P_WAIT_FETCH;
        send_control(oc, FETCH, page, s.e.version);
        return;
    }

    // Write (or free, which behaves like a write by nobody: every copy goes away).
    if (s.e.state == D_MODIFIED) {
        if (s.e.owner == r.requester) {  // it already owns the page; nothing to move
            send_control(r.conn, UPGRADE_ACK, page, s.e.version);
            finish(page, s);
            return;
        }
        auto oc = conn_of(s.e.owner);
        if (!oc) {
            LOG_ERROR("page %u: owner %u unreachable, page is LOST", page, s.e.owner);
            s.lost = true;
            return fail_current(page, s, DSM_ENODEDOWN);
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
        if (r.on_free) complete_free(page, s);
        else grant_write(page, s, nullptr);
        finish(page, s);
    } else {
        s.phase = P_WAIT_INV;
    }
}

// The previous owner/sharers are gone: hand the page to s.cur.requester in M.
void Directory::grant_write(page_id_t page, Slot& s, const uint8_t* data) {
    const Req& r = s.cur;
    if (is_down(r.requester)) {  // died while we were collecting ACKs: keep the home's bytes
        s.e.state = D_SHARED;
        s.e.owner = NODE_NONE;
        s.e.copyset = 0;
        return;
    }
    bool had_copy = (s.e.copyset & bit(r.requester)) != 0 && s.e.state == D_SHARED;
    s.e.version++;
    s.e.state = D_MODIFIED;
    s.e.owner = r.requester;
    s.e.copyset = bit(r.requester);
    if (had_copy) send_control(r.conn, UPGRADE_ACK, page, s.e.version);  // its S bytes are current
    else send_data(page, r, s.e.version, true, data);
}

// Every copy has been invalidated: zero the home's bytes so the next allocation starts clean.
void Directory::complete_free(page_id_t page, Slot& s) {
    std::vector<uint8_t> zeros(mem_.page_size(), 0);
    mem_.store_page(page, zeros.data());
    s.e.state = D_SHARED;
    s.e.owner = NODE_NONE;
    s.e.copyset = 0;
    s.e.version++;  // replies still in flight for the old contents are older than this
    s.lost = false;
    auto done = std::move(s.cur.on_free);
    s.cur.on_free = nullptr;
    if (done) done();
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
        if (s->cur.on_free) complete_free(page, *s);
        else grant_write(page, *s, nullptr);
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
    if (r.on_free) {
        complete_free(page, *s);
    } else if (r.access == ACC_READ) {
        s->e.state = D_SHARED;
        s->e.copyset = bit(old_owner);  // FETCH leaves the old owner in S
        s->e.owner = NODE_NONE;
        if (!is_down(r.requester)) {
            s->e.copyset |= bit(r.requester);
            send_data(page, r, s->e.version, false, payload);
        }
    } else {
        s->e.state = D_SHARED;  // so grant_write sends full data, never an UPGRADE_ACK
        s->e.copyset = 0;
        grant_write(page, *s, payload);
    }
    finish(page, *s);
}

void Directory::on_node_down(node_id_t node) {
    {
        std::lock_guard<std::mutex> lk(conns_lock_);
        if (down_.size() <= node) down_.resize(node + 1, false);
        down_[node] = true;
        if (node < node_conn_.size()) node_conn_[node].reset();
    }
    if (my_id_ == NODE_NONE) return;
    page_id_t first = mem_.first_page_of(my_id_);
    for (uint32_t i = 0; i < slots_.size(); i++) {
        page_id_t page = first + i;
        Slot& s = slots_[i];
        std::lock_guard<std::mutex> lk(dir_lock(page));
        bool had_copy = (s.e.copyset & bit(node)) != 0;
        bool owned = s.e.state == D_MODIFIED && s.e.owner == node;
        if (!had_copy && !owned) continue;

        s.e.copyset &= ~bit(node);
        if (owned) {
            // The only up-to-date bytes were in that node's memory.
            s.lost = true;
            s.e.state = D_SHARED;
            s.e.owner = NODE_NONE;
            LOG_ERROR("page %u LOST: node %u held it in M", page, node);
            if (s.e.busy && s.phase == P_WAIT_FETCH) fail_current(page, s, DSM_ENODEDOWN);
        } else if (s.e.busy && s.phase == P_WAIT_INV && --s.acks_left == 0) {
            // The ACK it owed will never come; its copy is gone, which is what the ACK meant.
            if (s.cur.on_free) complete_free(page, s);
            else grant_write(page, s, nullptr);
            finish(page, s);
        }
    }
}

void Directory::sweep(uint32_t timeout_ms) {
    if (my_id_ == NODE_NONE) return;
    uint64_t now = now_ms();
    page_id_t first = mem_.first_page_of(my_id_);
    for (uint32_t i = 0; i < slots_.size(); i++) {
        Slot& s = slots_[i];
        page_id_t page = first + i;
        std::lock_guard<std::mutex> lk(dir_lock(page));
        if (!s.e.busy || s.phase == P_IDLE || now - s.started_ms < timeout_ms) continue;
        LOG_WARN("page %u: node did not answer within %u ms, request from node %u abandoned", page, timeout_ms,
                 s.cur.requester);
        // The directory state is left as it was: copies that never acknowledged are still
        // listed, so a later write invalidates them again.
        fail_current(page, s, DSM_ETIMEOUT);
    }
}
