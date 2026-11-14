#include "alloc.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "log.h"

namespace {

uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}


}  // namespace

struct Allocator::FreeJob {
    std::atomic<uint32_t> left;
    std::shared_ptr<Connection> conn;
    Header req;
    std::vector<uint8_t> reply;
};

void Allocator::init(node_id_t my_id) {
    my_id_ = my_id;
    std::lock_guard<std::mutex> lk(lock_);
    used_.assign(mem_.pages_per_node(), 0);
    pieces_.clear();
}

uint32_t Allocator::free_pages() {
    std::lock_guard<std::mutex> lk(lock_);
    if (used_.empty()) return mem_.pages_per_node();  // not initialised yet: nothing is taken
    return static_cast<uint32_t>(std::count(used_.begin(), used_.end(), 0));
}

std::vector<Allocator::Piece> Allocator::pieces() {
    std::lock_guard<std::mutex> lk(lock_);
    return pieces_;
}

bool Allocator::piece_of(page_id_t page, Piece& out) {
    std::lock_guard<std::mutex> lk(lock_);
    for (const Piece& p : pieces_)
        if (page >= p.rec.first_page && page < p.rec.first_page + p.rec.n_pages) {
            out = p;
            return true;
        }
    return false;
}

bool Allocator::alive(node_id_t n) {
    NodeInfo info{};
    return ms_.node_info(n, info) && info.state != DEAD && info.state != LEFT;
}

// ---------------------------------------------------------------- home side

// Takes pages out of this node's range. Returns DSM_OK with the pages reserved in
// (first, count), DSM_ENOMEM if they are not there.
int Allocator::reserve(const Request& r, uint16_t flags, node_id_t requester, uint32_t& first, uint32_t& count) {
    const uint32_t P = mem_.pages_per_node();
    const page_id_t base = mem_.first_page_of(my_id_);
    uint32_t off = 0, cnt = 0;
    uint32_t start = r.alloc_start;
    {
        std::lock_guard<std::mutex> lk(lock_);
        if (used_.empty()) return DSM_ENOMEM;  // a request that beat init() here
        if (flags & F_ALLOC_ANY) {
            cnt = r.n_pages;
            // Even nodes pack small blocks at the start of their range and odd nodes at the
            // end. An allocation that spans ranges needs the free tail of one range and the
            // free start of the next, so this keeps those two ends free for the pair (2k, 2k+1).
            bool found = false;
            uint32_t run = 0;
            if (my_id_ % 2 == 0) {
                for (uint32_t i = 0; i < P && !found; i++) {
                    run = used_[i] ? 0 : run + 1;
                    if (run == cnt) { off = i + 1 - cnt; found = true; }
                }
            } else {
                for (uint32_t i = P; i-- > 0 && !found;) {
                    run = used_[i] ? 0 : run + 1;
                    if (run == cnt) { off = i; found = true; }
                }
            }
            if (!found) return DSM_ENOMEM;
            start = base + off;
        } else if (flags & F_ALLOC_TAIL) {
            uint32_t tail = 0;
            while (tail < P && !used_[P - 1 - tail]) tail++;
            if (tail == 0) return DSM_ENOMEM;
            cnt = std::min(tail, r.n_pages);
            off = P - cnt;  // the end of the range, so the next range continues it
            start = base + off;
        } else {
            if (r.first_page < base || r.n_pages == 0 || r.first_page - base + r.n_pages > P) return DSM_EINVAL;
            off = r.first_page - base;
            cnt = r.n_pages;
            for (uint32_t i = off; i < off + cnt; i++)
                if (used_[i]) return DSM_ENOMEM;
        }

        for (uint32_t i = off; i < off + cnt; i++) {
            used_[i] = 1;
            page_id_t p = base + i;
            std::lock_guard<std::mutex> pl(mem_.page_lock(p));
            PageEntry& e = mem_.entry(p);
            e.flags |= PG_ALLOCATED;
            e.alloc_id = r.alloc_id;
            e.alloc_order = p - start;
        }
        Piece piece{};
        piece.rec.alloc_id = r.alloc_id;
        piece.rec.owner_node = requester;
        piece.rec.owner_pid = r.pid;
        piece.rec.first_page = base + off;
        piece.rec.n_pages = cnt;
        piece.rec.size_bytes = r.size;
        piece.rec.created_ms = now_ms();
        memcpy(piece.rec.name, r.name, sizeof piece.rec.name);
        piece.alloc_start = start;
        pieces_.push_back(piece);
    }
    dir_.set_allocated(base + off, cnt, true);
    first = base + off;
    count = cnt;
    return DSM_OK;
}

void Allocator::on_alloc_request(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload) {
    ByteReader rd(payload, h.payload_len);
    Request r;
    r.alloc_id = rd.u32();
    r.first_page = rd.u32();
    r.n_pages = rd.u32();
    r.pid = static_cast<int32_t>(rd.u32());
    r.alloc_start = rd.u32();
    r.size = rd.u64();
    rd.bytes(r.name, sizeof r.name);
    if (!rd.ok() || r.n_pages == 0) {
        cp_.reply(conn, h, ALLOC_REPLY, DSM_EPROTO);
        return;
    }
    uint32_t first = 0, count = 0;
    int rc = reserve(r, h.flags, h.src_node, first, count);
    std::vector<uint8_t> out;
    ByteWriter w(out);
    w.u32(first);
    w.u32(count);
    if (rc == DSM_OK)
        LOG_INFO("alloc %08x: pages %u..%u reserved for node %u", r.alloc_id, first, first + count - 1, h.src_node);
    cp_.reply(conn, h, ALLOC_REPLY, rc, out.data(), out.size());
}

void Allocator::on_free(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload) {
    ByteReader rd(payload, h.payload_len);
    uint32_t alloc_id = rd.u32(), first = rd.u32(), n_pages = rd.u32();
    if (!rd.ok()) {
        cp_.reply(conn, h, FREE_ACK, DSM_EPROTO);
        return;
    }

    Piece piece{};
    {
        std::lock_guard<std::mutex> lk(lock_);
        auto it = std::find_if(pieces_.begin(), pieces_.end(), [&](const Piece& p) {
            return p.rec.first_page == first && (alloc_id == 0 || p.rec.alloc_id == alloc_id);
        });
        // n_pages == 0 means "the allocation that starts here": a piece in the middle of an
        // allocation must not be freed by itself.
        if (it == pieces_.end() || (n_pages == 0 && it->alloc_start != first)) {
            cp_.reply(conn, h, FREE_ACK, DSM_ENOTFOUND);
            return;
        }
        if (n_pages != 0 && n_pages != it->rec.n_pages) {
            cp_.reply(conn, h, FREE_ACK, DSM_EINVAL);
            return;
        }
        piece = *it;
        pieces_.erase(it);
        const page_id_t base = mem_.first_page_of(my_id_);
        for (uint32_t i = 0; i < piece.rec.n_pages; i++) {
            page_id_t p = piece.rec.first_page + i;
            used_[p - base] = 0;
            std::lock_guard<std::mutex> pl(mem_.page_lock(p));
            PageEntry& e = mem_.entry(p);
            e.flags &= ~PG_ALLOCATED;
            e.alloc_id = 0;
            e.alloc_order = 0;
        }
    }

    auto job = std::make_shared<FreeJob>();
    job->left = piece.rec.n_pages;
    job->conn = conn;
    job->req = h;
    ByteWriter w(job->reply);
    w.u32(piece.rec.alloc_id);
    w.u64(piece.rec.size_bytes);
    w.u32(piece.rec.n_pages);
    w.u32(piece.alloc_start);
    LOG_INFO("alloc %08x: pages %u..%u freed", piece.rec.alloc_id, piece.rec.first_page,
             piece.rec.first_page + piece.rec.n_pages - 1);
    // The ACK goes out when the last page is clean, so a returned dsm_free() means the pages
    // really are free (no stale copy survives anywhere).
    for (uint32_t i = 0; i < piece.rec.n_pages; i++)
        dir_.free_page(piece.rec.first_page + i, [this, job] {
            if (--job->left == 0) cp_.reply(job->conn, job->req, FREE_ACK, DSM_OK, job->reply.data(), job->reply.size());
        });
}

void Allocator::on_message(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload) {
    switch (h.type) {
        case ALLOC_REQUEST: on_alloc_request(conn, h, payload); break;
        case FREE: on_free(conn, h, payload); break;
        default: break;
    }
}

// ------------------------------------------------------------- requester side

int Allocator::ask_home(node_id_t home, const Request& r, uint16_t flags, uint32_t& first, uint32_t& count) {
    std::vector<uint8_t> payload;
    ByteWriter w(payload);
    w.u32(r.alloc_id);
    w.u32(r.first_page);
    w.u32(r.n_pages);
    w.u32(static_cast<uint32_t>(r.pid));
    w.u32(r.alloc_start);
    w.u64(r.size);
    w.bytes(r.name, sizeof r.name);

    Header h;
    h.type = ALLOC_REQUEST;
    h.flags = flags;
    ControlPlane::Reply rep;
    int rc = cp_.call(home, h, payload.data(), payload.size(), rep, cfg_.request_timeout_ms);
    if (rc != DSM_OK) return rc;
    ByteReader rd(rep.payload.data(), rep.payload.size());
    first = rd.u32();
    count = rd.u32();
    return rd.ok() ? DSM_OK : DSM_EPROTO;
}

int Allocator::free_piece(node_id_t home, uint32_t alloc_id, uint32_t first, uint32_t n, ControlPlane::Reply& out) {
    std::vector<uint8_t> payload;
    ByteWriter w(payload);
    w.u32(alloc_id);
    w.u32(first);
    w.u32(n);
    Header h;
    h.type = FREE;
    return cp_.call(home, h, payload.data(), payload.size(), out, cfg_.request_timeout_ms * 4);
}

void Allocator::give_back(const std::vector<std::pair<uint32_t, uint32_t>>& pieces, uint32_t alloc_id) {
    for (const auto& pc : pieces) {
        ControlPlane::Reply rep;
        free_piece(mem_.home_of(pc.first), alloc_id, pc.first, pc.second, rep);
    }
}

int64_t Allocator::allocate(size_t size, const std::string& name, uint32_t* alloc_id_out) {
    const uint64_t ps = cfg_.page_size;
    const uint32_t n = static_cast<uint32_t>((size + ps - 1) / ps);
    const uint32_t P = mem_.pages_per_node();
    if (size == 0 || n == 0) return DSM_EINVAL;

    Request r;
    r.alloc_id = ((my_id_ + 1u) << 24) | (next_id_.fetch_add(1) & 0xFFFFFF);
    r.pid = getpid();
    r.size = size;
    if (alloc_id_out) *alloc_id_out = r.alloc_id;
    strncpy(r.name, name.c_str(), sizeof r.name - 1);

    const size_t known = ms_.snapshot().size();
    int last_err = DSM_ENOMEM;

    // 1. All inside one range: the cheapest case, and the own range is tried first.
    if (n <= P) {
        for (size_t k = 0; k < known; k++) {
            node_id_t node = static_cast<node_id_t>((my_id_ + k) % known);
            if (!alive(node)) continue;
            Request q = r;
            q.n_pages = n;
            uint32_t first = 0, count = 0;
            int rc = ask_home(node, q, F_ALLOC_ANY, first, count);
            if (rc == DSM_OK) return first;
            if (rc != DSM_ENOMEM) last_err = rc;
        }
    }

    // 2. Spanning: the tail of one range and the start of the following ones.
    for (size_t k = 0; k < known; k++) {
        node_id_t s = static_cast<node_id_t>((my_id_ + k) % known);
        if (!alive(s)) continue;
        Request q = r;
        q.n_pages = n;
        uint32_t first = 0, count = 0;
        int rc = ask_home(s, q, F_ALLOC_TAIL, first, count);
        if (rc != DSM_OK) {
            if (rc != DSM_ENOMEM) last_err = rc;
            continue;
        }
        std::vector<std::pair<uint32_t, uint32_t>> done{{first, count}};
        const uint32_t start = first;
        uint32_t remaining = n - count;
        bool failed = false;
        for (node_id_t j = s + 1; remaining > 0; j++) {
            if (j >= known || !alive(j)) {
                failed = true;
                break;
            }
            Request e = r;
            e.first_page = mem_.first_page_of(j);
            e.n_pages = std::min(remaining, P);
            e.alloc_start = start;
            rc = ask_home(j, e, 0, first, count);
            if (rc != DSM_OK) {
                if (rc != DSM_ENOMEM) last_err = rc;
                failed = true;
                break;
            }
            done.push_back({first, count});
            remaining -= count;
        }
        if (!failed) return start;
        give_back(done, r.alloc_id);
    }
    return last_err;
}

int Allocator::release(page_id_t first_page, uint32_t& alloc_id_out) {
    if (!mem_.valid_page(first_page)) return DSM_ENOTFOUND;
    ControlPlane::Reply rep;
    int rc = free_piece(mem_.home_of(first_page), 0, first_page, 0, rep);
    if (rc != DSM_OK) return rc;

    ByteReader rd(rep.payload.data(), rep.payload.size());
    uint32_t alloc_id = rd.u32();
    uint64_t size = rd.u64();
    uint32_t piece_pages = rd.u32();
    if (!rd.ok()) return DSM_EPROTO;
    alloc_id_out = alloc_id;

    const uint64_t ps = cfg_.page_size;
    const uint32_t P = mem_.pages_per_node();
    uint32_t remaining = static_cast<uint32_t>((size + ps - 1) / ps) - piece_pages;
    page_id_t p = first_page + piece_pages;
    int result = DSM_OK;
    while (remaining > 0) {
        node_id_t node = mem_.home_of(p);
        uint32_t range_end = mem_.first_page_of(node) + P;
        uint32_t take = std::min(remaining, range_end - p);
        ControlPlane::Reply r2;
        rc = free_piece(node, alloc_id, p, take, r2);
        if (rc != DSM_OK) result = rc;  // keep going: free what can still be freed
        p += take;
        remaining -= take;
    }
    return result;
}
