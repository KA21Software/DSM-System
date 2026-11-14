#include "control.h"

#include <arpa/inet.h>

#include <chrono>

#include "log.h"

std::shared_ptr<Connection> ControlPlane::connection_to(node_id_t node) {
    {
        std::lock_guard<std::mutex> lk(conn_lock_);
        if (node < conn_.size() && conn_[node] && !conn_[node]->closed.load()) return conn_[node];
    }
    NodeInfo info{};
    if (!ms_.node_info(node, info) || info.state == DEAD || info.state == LEFT) return nullptr;
    in_addr a{};
    a.s_addr = htonl(info.ipv4);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a, ip, sizeof ip);

    auto conn = ms_.ctrl_net().connect_to(ip, info.port_ctrl, cfg_.connect_timeout_ms);
    if (!conn) return nullptr;
    std::lock_guard<std::mutex> lk(conn_lock_);
    if (conn_.size() <= node) conn_.resize(node + 1);
    conn_[node] = conn;
    return conn;
}

int ControlPlane::send(node_id_t dst, Header h, const void* payload, size_t len) {
    h.src_node = ms_.my_id();
    h.dst_node = dst;
    if (h.seq == 0) h.seq = seq_.fetch_add(1);
    for (int attempt = 0; attempt < 2; attempt++) {
        auto conn = connection_to(dst);
        if (!conn) return DSM_ENODEDOWN;
        if (ms_.ctrl_net().send(conn, h, payload, len) == DSM_OK) return DSM_OK;
    }
    return DSM_ENET;
}

int ControlPlane::call(node_id_t dst, Header h, const void* payload, size_t len, Reply& out, uint32_t timeout_ms) {
    h.seq = seq_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(pend_lock_);
        pending_[h.seq].dst = dst;
    }
    int rc = send(dst, h, payload, len);
    if (rc != DSM_OK) {
        std::lock_guard<std::mutex> lk(pend_lock_);
        pending_.erase(h.seq);
        return rc;
    }

    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lk(pend_lock_);
    auto down_since = clock::time_point::max();
    for (;;) {
        Pending& p = pending_[h.seq];
        if (p.done) {
            int status = p.status;
            out.h = p.h;
            out.payload = std::move(p.payload);
            pending_.erase(h.seq);
            return status;
        }
        pend_cv_.wait_for(lk, std::chrono::milliseconds(100));
        if (!pending_[h.seq].done) {
            NodeInfo info{};
            bool down = !ms_.node_info(dst, info) || info.state == DEAD || info.state == LEFT;
            if (!down) down_since = clock::time_point::max();
            else if (down_since == clock::time_point::max()) down_since = clock::now();
            // A reply may still be on its way when the node's LEAVE is seen first (they travel
            // on different connections), so a down node gets a short grace period.
            bool give_up = down && clock::now() - down_since >= std::chrono::milliseconds(400);
            if (give_up || (timeout_ms && clock::now() >= deadline)) {
                pending_.erase(h.seq);
                return give_up ? DSM_ENODEDOWN : DSM_ETIMEOUT;
            }
        }
    }
}

void ControlPlane::broadcast(Header h, const void* payload, size_t len, bool include_self) {
    for (const NodeInfo& n : ms_.snapshot()) {
        if (n.state == DEAD || n.state == LEFT) continue;
        if (n.node_id == ms_.my_id() && !include_self) continue;
        Header copy = h;
        copy.seq = 0;
        send(n.node_id, copy, payload, len);
    }
}

void ControlPlane::reply(const std::shared_ptr<Connection>& conn, const Header& req, kadp_type type, int status,
                         const void* payload, size_t len) {
    Header h;
    h.type = type;
    h.seq = req.seq;
    h.flags = F_IS_REPLY;
    h.src_node = ms_.my_id();
    h.dst_node = req.src_node;
    h.status = static_cast<int16_t>(status);
    ms_.ctrl_net().send(conn, h, payload, len);
}

bool ControlPlane::on_reply(const Header& h, const uint8_t* payload) {
    std::lock_guard<std::mutex> lk(pend_lock_);
    auto it = pending_.find(h.seq);
    if (it == pending_.end() || it->second.done) return false;
    Pending& p = it->second;
    p.done = true;
    p.status = h.status;
    p.h = h;
    if (h.payload_len) p.payload.assign(payload, payload + h.payload_len);
    pend_cv_.notify_all();
    return true;
}

void ControlPlane::fail_pending_for(node_id_t node) {
    {
        std::lock_guard<std::mutex> lk(pend_lock_);
        for (auto& kv : pending_)
            if (kv.second.dst == node && !kv.second.done) {
                kv.second.done = true;
                kv.second.status = DSM_ENODEDOWN;
            }
        pend_cv_.notify_all();
    }
    std::lock_guard<std::mutex> lk(conn_lock_);
    if (node < conn_.size() && conn_[node]) {
        conn_[node]->close_now();
        conn_[node].reset();
    }
}
