#include "membership.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <algorithm>
#include <chrono>
#include <random>

#include "log.h"

namespace {

uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// NodeInfo.ipv4 is kept in host byte order everywhere in memory; this converts it to a
// dotted string for connect_to() and for log lines. The wire format (write_node_info) is
// unrelated: it serialises the same host-order value field by field, big-endian, like any
// other integer in the protocol (section 6.2).
std::string ipv4_to_string(uint32_t host_order_ip) {
    in_addr a{};
    a.s_addr = htonl(host_order_ip);
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a, buf, sizeof buf);
    return buf;
}

// Picks this machine's advertisable IPv4 (section 1 of
// documentacion/.../Entorno_Laboratorio.md: the lab's real interface, never loopback or a
// virtual bridge such as docker0, which other machines cannot reach). Falls back to loopback
// for same-machine testing when no such interface exists.
uint32_t detect_local_ipv4() {
    uint32_t chosen = INADDR_LOOPBACK;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* it = list; it; it = it->ifa_next) {
            if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
            if (it->ifa_flags & IFF_LOOPBACK) continue;
            std::string name = it->ifa_name;
            if (name.rfind("docker", 0) == 0 || name.rfind("br-", 0) == 0 || name.rfind("veth", 0) == 0) continue;
            chosen = ntohl(reinterpret_cast<sockaddr_in*>(it->ifa_addr)->sin_addr.s_addr);
            break;
        }
        freeifaddrs(list);
    }
    return chosen;
}

// True when `host` names one of this machine's own interfaces: the join probe (section 7.1)
// must skip its own slot there, or it would send itself a JOIN.
bool is_local_host(const std::string& host) {
    if (host == "127.0.0.1" || host == "localhost") return true;
    in_addr target{};
    if (inet_pton(AF_INET, host.c_str(), &target) != 1) return false;  // a hostname: not resolved here

    bool found = false;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* it = list; it && !found; it = it->ifa_next) {
            if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
            if (reinterpret_cast<sockaddr_in*>(it->ifa_addr)->sin_addr.s_addr == target.s_addr) found = true;
        }
        freeifaddrs(list);
    }
    return found;
}

// Wire layout of one NodeInfo (table 7.1), without the struct's 7 trailing padding bytes:
// the payload is read back field by field, so there is nothing to pad for alignment.
void write_node_info(ByteWriter& w, const NodeInfo& n) {
    w.u16(n.node_id);
    w.u8(n.state);
    w.u8(n.slot);
    w.u32(n.ipv4);
    w.u16(n.port_data);
    w.u16(n.port_ctrl);
    w.u16(n.port_hb);
    w.u16(n.port_web);
    w.u32(n.mem_pages);
    w.u32(n.free_pages);
    w.u64(n.joined_ms);
    w.u64(n.last_hb_ms);
    w.u8(n.missed_hb);
}

bool read_node_info(ByteReader& r, NodeInfo& n) {
    n.node_id = r.u16();
    n.state = r.u8();
    n.slot = r.u8();
    n.ipv4 = r.u32();
    n.port_data = r.u16();
    n.port_ctrl = r.u16();
    n.port_hb = r.u16();
    n.port_web = r.u16();
    n.mem_pages = r.u32();
    n.free_pages = r.u32();
    n.joined_ms = r.u64();
    n.last_hb_ms = r.u64();
    n.missed_hb = r.u8();
    return r.ok();
}

}  // namespace

NodePorts node_ports(const Config& cfg, uint8_t slot) {
    uint16_t base = static_cast<uint16_t>(cfg.port_base + cfg.port_stride * slot);
    return {static_cast<uint16_t>(base + 0), static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 2),
            static_cast<uint16_t>(base + 3)};
}

bool Membership::bind_slot() {
    for (uint32_t s = 0; s < cfg_.max_nodes_per_host; s++) {
        NodePorts ports = node_ports(cfg_, static_cast<uint8_t>(s));
        if (ctrl_net_.listen(ports.ctrl) != DSM_OK) continue;
        if (hb_net_.listen(ports.hb) != DSM_OK) {
            ctrl_net_.stop();  // releases the control port so the next slot can try it cleanly
            continue;
        }
        slot_ = static_cast<uint8_t>(s);
        LOG_INFO("bound slot %u (control port %u, heartbeat port %u)", slot_, ports.ctrl, ports.hb);
        return true;
    }
    LOG_ERROR("no free slot in 0..%u on this host", cfg_.max_nodes_per_host - 1);
    return false;
}

bool Membership::found_cluster() {
    NodePorts ports = node_ports(cfg_, slot_);
    NodeInfo info{};
    info.node_id = 0;
    info.state = ALIVE;
    info.slot = slot_;
    info.ipv4 = local_ipv4_;
    info.port_data = ports.data;
    info.port_ctrl = ports.ctrl;
    info.port_hb = ports.hb;
    info.port_web = ports.web;
    info.mem_pages = cfg_.pages_per_node();
    info.free_pages = info.mem_pages;
    info.joined_ms = info.last_hb_ms = now_ms();

    std::unique_lock<std::shared_mutex> lk(nodes_lock_);
    nodes_.clear();
    nodes_.push_back(info);
    my_id_ = 0;
    lk.unlock();
    LOG_INFO("no existing cluster found: founding it as node 0");
    return true;
}

// Returns DSM_OK on success, DSM_ENODEDOWN if nobody answered after two full passes (the
// caller should then found_cluster()), or a definitive rejection (DSM_ECONFIG, DSM_EFULL)
// from a coordinator that does exist, which the caller must not paper over by founding
// a second, inconsistent cluster.
int Membership::join_existing() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> jitter_ms(0, 500);

    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt == 1) std::this_thread::sleep_for(std::chrono::milliseconds(jitter_ms(gen)));

        for (const auto& host : cfg_.hosts) {
            for (uint32_t s = 0; s < cfg_.max_nodes_per_host; s++) {
                if (is_local_host(host) && s == slot_) continue;  // that candidate would be myself

                NodePorts ports = node_ports(cfg_, static_cast<uint8_t>(s));
                auto conn = ctrl_net_.connect_to(host, ports.ctrl, cfg_.connect_timeout_ms);
                if (!conn) continue;  // nobody there (yet); try the next candidate

                uint32_t seq = seq_.fetch_add(1);
                {
                    std::lock_guard<std::mutex> lk(join_wait_lock_);
                    join_wait_pending_ = true;
                    join_wait_seq_ = seq;
                }

                std::vector<uint8_t> payload;
                ByteWriter w(payload);
                w.u32(local_ipv4_);
                w.u8(slot_);
                w.u32(cfg_.pages_per_node());
                w.u32(config_hash(cfg_));

                Header h;
                h.type = JOIN;
                h.seq = seq;
                if (ctrl_net_.send(conn, h, payload.data(), payload.size()) != DSM_OK) continue;

                std::unique_lock<std::mutex> lk(join_wait_lock_);
                bool answered = join_wait_cv_.wait_for(lk, std::chrono::milliseconds(cfg_.request_timeout_ms),
                                                        [&] { return !join_wait_pending_; });
                if (!answered) {
                    join_wait_pending_ = false;  // stop matching a late reply to this abandoned attempt
                    lk.unlock();
                    LOG_INFO("no answer from %s:%u, trying next candidate", host.c_str(), ports.ctrl);
                    conn->close_now();
                    continue;
                }
                int status = join_wait_status_;
                lk.unlock();

                if (status == DSM_ECONFIG || status == DSM_EFULL) {
                    LOG_ERROR("join rejected by %s:%u: status %d", host.c_str(), ports.ctrl, status);
                    return status;
                }
                if (status != DSM_OK) {
                    LOG_INFO("%s:%u is not the coordinator, trying next candidate", host.c_str(), ports.ctrl);
                    conn->close_now();
                    continue;
                }

                std::unique_lock<std::shared_mutex> nlk(nodes_lock_);
                nodes_ = join_wait_table_;
                my_id_ = join_wait_id_;
                size_t known = nodes_.size();
                nlk.unlock();
                LOG_INFO("joined the cluster as node %u (%zu known nodes)", my_id_, known);
                return DSM_OK;
            }
        }
    }
    return DSM_ENODEDOWN;
}

int Membership::join() {
    local_ipv4_ = detect_local_ipv4();
    if (!bind_slot()) return DSM_ENET;

    ctrl_net_.set_handler(msg_class(JOIN), [this](const std::shared_ptr<Connection>& c, const Header& h,
                                                   const uint8_t* p) { on_ctrl_message(c, h, p); });
    ctrl_net_.set_handler(msg_class(KADP_ERROR), [this](const std::shared_ptr<Connection>& c, const Header& h,
                                                         const uint8_t* p) { on_ctrl_error(c, h, p); });
    hb_net_.set_handler(msg_class(HEARTBEAT), [this](const std::shared_ptr<Connection>& c, const Header& h,
                                                      const uint8_t* p) { on_heartbeat(c, h, p); });
    ctrl_net_.start();
    hb_net_.start();

    int rc = join_existing();
    if (rc == DSM_ENODEDOWN) {
        if (!found_cluster()) return DSM_ENET;
    } else if (rc != DSM_OK) {
        return rc;
    }

    running_ = true;
    hb_thread_ = std::thread(&Membership::heartbeat_loop, this);
    return DSM_OK;
}

void Membership::stop() {
    running_ = false;
    if (hb_thread_.joinable()) hb_thread_.join();
    ctrl_net_.stop();
    hb_net_.stop();
}

bool Membership::is_coordinator() const {
    std::shared_lock<std::shared_mutex> lk(nodes_lock_);
    if (my_id_ == NODE_NONE || my_id_ >= nodes_.size()) return false;
    for (node_id_t id = 0; id < my_id_; id++)
        if (nodes_[id].state == ALIVE) return false;  // someone with a lower id outranks us
    return nodes_[my_id_].state == ALIVE;
}

bool Membership::node_info(node_id_t id, NodeInfo& out) const {
    std::shared_lock<std::shared_mutex> lk(nodes_lock_);
    if (id >= nodes_.size()) return false;
    out = nodes_[id];
    return true;
}

std::vector<NodeInfo> Membership::snapshot() const {
    std::shared_lock<std::shared_mutex> lk(nodes_lock_);
    return nodes_;
}

node_id_t Membership::add_node_locked(const NodeInfo& info) {
    if (info.node_id == nodes_.size()) {
        nodes_.push_back(info);
    } else if (info.node_id < nodes_.size()) {
        nodes_[info.node_id] = info;  // a duplicate JOIN_ACK/NODE_ANNOUNCE: refresh in place
    } else {
        LOG_WARN("node %u announced out of order (only know %zu nodes), ignored", info.node_id, nodes_.size());
    }
    return info.node_id;
}

void Membership::on_ctrl_message(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload) {
    switch (h.type) {
        case JOIN:
            handle_join(conn, h, payload);
            break;
        case JOIN_ACK:
            handle_join_ack(h, payload);
            break;
        case NODE_ANNOUNCE:
            handle_node_announce(h, payload);
            break;
        default:
            LOG_DEBUG("ignored control message type 0x%02x", h.type);
            break;
    }
}

void Membership::on_ctrl_error(const std::shared_ptr<Connection>&, const Header& h, const uint8_t*) {
    std::lock_guard<std::mutex> lk(join_wait_lock_);
    if (join_wait_pending_ && h.seq == join_wait_seq_) {
        join_wait_status_ = h.status;
        join_wait_pending_ = false;
        join_wait_cv_.notify_all();
    }
}

void Membership::handle_join(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload) {
    ByteReader r(payload, h.payload_len);
    uint32_t ipv4 = r.u32();
    uint8_t slot = r.u8();
    uint32_t mem_pages = r.u32();
    uint32_t hash = r.u32();
    if (!r.ok()) {
        LOG_WARN("malformed JOIN, ignored");
        return;
    }

    auto reject = [&](int status) {
        Header e;
        e.type = KADP_ERROR;
        e.seq = h.seq;
        e.src_node = my_id_;
        e.status = static_cast<int16_t>(status);
        ctrl_net_.send(conn, e, nullptr, 0);
    };
    auto send_ack = [&](node_id_t id, const std::vector<NodeInfo>& table) {
        std::vector<uint8_t> ack_payload;
        ByteWriter w(ack_payload);
        w.u16(id);
        w.u16(static_cast<uint16_t>(table.size()));
        w.u16(0);  // n_names: no names table yet (E2 scope)
        for (const auto& n : table) write_node_info(w, n);
        Header ack;
        ack.type = JOIN_ACK;
        ack.seq = h.seq;
        ack.src_node = my_id_;
        ctrl_net_.send(conn, ack, ack_payload.data(), ack_payload.size());
    };

    // E2 simplification: no multi-hop FORWARDED relay (design section 7.1 step 5). The
    // joiner's own probe (join_existing()) tries every candidate until it reaches the real
    // coordinator directly, so this rejection is enough to make it move on.
    if (!is_coordinator()) {
        LOG_INFO("not the coordinator, rejecting JOIN from slot %u", slot);
        reject(DSM_ENODEDOWN);
        return;
    }
    if (hash != config_hash(cfg_)) {
        LOG_WARN("JOIN from slot %u rejected: config_hash mismatch", slot);
        reject(DSM_ECONFIG);
        return;
    }

    std::unique_lock<std::shared_mutex> lk(nodes_lock_);

    // A retry can cross with a reply that was just slow, not lost (e.g. request_timeout_ms
    // is tight, or the network briefly stalled): the same (ipv4, slot) means the same
    // physical node asking again, so it gets its existing id back instead of a second one.
    for (const auto& n : nodes_) {
        if (n.ipv4 != ipv4 || n.slot != slot || n.state == LEFT) continue;
        node_id_t existing_id = n.node_id;
        if (static_cast<size_t>(existing_id) >= ctrl_conn_.size()) ctrl_conn_.resize(existing_id + 1);
        ctrl_conn_[existing_id] = conn;  // the retry's fresh connection replaces the old one
        std::vector<NodeInfo> table_copy = nodes_;
        lk.unlock();
        LOG_INFO("node %u re-joined (a retry of an earlier reply), resending its assignment", existing_id);
        send_ack(existing_id, table_copy);
        return;
    }

    if (nodes_.size() >= cfg_.max_nodes) {
        lk.unlock();
        LOG_WARN("JOIN from slot %u rejected: cluster full (%u ids used)", slot, cfg_.max_nodes);
        reject(DSM_EFULL);
        return;
    }

    NodePorts ports = node_ports(cfg_, slot);
    NodeInfo info{};
    info.node_id = static_cast<node_id_t>(nodes_.size());
    info.state = ALIVE;
    info.slot = slot;
    info.ipv4 = ipv4;
    info.port_data = ports.data;
    info.port_ctrl = ports.ctrl;
    info.port_hb = ports.hb;
    info.port_web = ports.web;
    info.mem_pages = mem_pages;
    info.free_pages = mem_pages;
    info.joined_ms = info.last_hb_ms = now_ms();

    node_id_t new_id = add_node_locked(info);
    if (static_cast<size_t>(new_id) >= ctrl_conn_.size()) ctrl_conn_.resize(new_id + 1);
    ctrl_conn_[new_id] = conn;  // kept open: the only place NODE_ANNOUNCE can reach this node later
    std::vector<NodeInfo> table_copy = nodes_;
    lk.unlock();

    LOG_INFO("node %u joined (ip %s, slot %u)", new_id, ipv4_to_string(ipv4).c_str(), slot);
    send_ack(new_id, table_copy);

    // table_copy, just sent above, already includes the new node, so it does not need this.
    std::vector<uint8_t> announce_payload;
    ByteWriter aw(announce_payload);
    write_node_info(aw, info);
    Header announce;
    announce.type = NODE_ANNOUNCE;
    announce.seq = seq_.fetch_add(1);
    announce.src_node = my_id_;

    std::shared_lock<std::shared_mutex> rlk(nodes_lock_);
    for (node_id_t id = 0; id < static_cast<node_id_t>(ctrl_conn_.size()); id++) {
        if (id == new_id || !ctrl_conn_[id]) continue;
        ctrl_net_.send(ctrl_conn_[id], announce, announce_payload.data(), announce_payload.size());
    }
}

void Membership::handle_join_ack(const Header& h, const uint8_t* payload) {
    std::lock_guard<std::mutex> lk(join_wait_lock_);
    if (!join_wait_pending_ || h.seq != join_wait_seq_) return;  // not ours, or we already gave up

    ByteReader r(payload, h.payload_len);
    uint16_t assigned_id = r.u16();
    uint16_t n_nodes = r.u16();
    r.u16();  // n_names: no names table yet (E2 scope)

    std::vector<NodeInfo> table;
    table.reserve(n_nodes);
    for (uint16_t i = 0; i < n_nodes && r.ok(); i++) {
        NodeInfo info{};
        if (!read_node_info(r, info)) break;
        table.push_back(info);
    }

    if (!r.ok() || table.size() != n_nodes) {
        join_wait_status_ = DSM_EPROTO;
    } else {
        join_wait_status_ = DSM_OK;
        join_wait_id_ = assigned_id;
        join_wait_table_ = std::move(table);
    }
    join_wait_pending_ = false;
    join_wait_cv_.notify_all();
}

void Membership::handle_node_announce(const Header& h, const uint8_t* payload) {
    ByteReader r(payload, h.payload_len);
    NodeInfo info{};
    if (!read_node_info(r, info) || !r.ok()) {
        LOG_WARN("malformed NODE_ANNOUNCE, ignored");
        return;
    }

    std::unique_lock<std::shared_mutex> lk(nodes_lock_);
    bool is_new = info.node_id == nodes_.size();
    add_node_locked(info);
    lk.unlock();

    if (is_new) LOG_INFO("node %u announced (ip %s, slot %u)", info.node_id, ipv4_to_string(info.ipv4).c_str(), info.slot);
}

void Membership::on_heartbeat(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload) {
    if (h.type != HEARTBEAT || h.src_node == NODE_NONE) return;
    ByteReader r(payload, h.payload_len);
    uint32_t free_pages = r.u32();
    r.u32();  // faults: no pager to report from yet (E2 scope)
    r.u64();  // sender's timestamp: unused, see the comment on last_hb_ms below
    if (!r.ok()) return;

    std::unique_lock<std::shared_mutex> lk(nodes_lock_);
    if (h.src_node >= nodes_.size()) return;  // from a node we have not been told about yet
    NodeInfo& n = nodes_[h.src_node];
    bool was_down = n.missed_hb > 0 || n.state != ALIVE;
    // Our own clock, not the sender's timestamp field: the two nodes' clocks are never
    // assumed to be in sync, and last_hb_ms only has to measure elapsed time on this node.
    n.last_hb_ms = now_ms();
    n.free_pages = free_pages;
    n.missed_hb = 0;
    n.state = ALIVE;

    if (h.src_node >= hb_conn_.size()) hb_conn_.resize(h.src_node + 1);
    if (!hb_conn_[h.src_node] || hb_conn_[h.src_node]->closed.load()) hb_conn_[h.src_node] = conn;
    lk.unlock();

    if (was_down) LOG_INFO("node %u is ALIVE again", h.src_node);
}

void Membership::send_heartbeats() {
    std::vector<NodeInfo> table = snapshot();
    // No allocator yet (E2 scope): the whole range this node owns is still free.
    uint32_t free_pages = cfg_.pages_per_node();

    std::vector<uint8_t> payload;
    ByteWriter w(payload);
    w.u32(free_pages);
    w.u32(0);  // faults: no pager to report from yet (E2 scope)
    w.u64(static_cast<uint64_t>(now_ms()));

    for (const auto& peer : table) {
        if (peer.node_id == my_id_ || peer.state == DEAD || peer.state == LEFT) continue;

        std::shared_ptr<Connection> conn;
        {
            std::shared_lock<std::shared_mutex> lk(nodes_lock_);
            if (peer.node_id < hb_conn_.size()) conn = hb_conn_[peer.node_id];
        }
        if (!conn || conn->closed.load()) {
            conn = hb_net_.connect_to(ipv4_to_string(peer.ipv4), peer.port_hb, cfg_.connect_timeout_ms);
            if (!conn) continue;  // they may be down; check_missed_heartbeats() will notice
            std::unique_lock<std::shared_mutex> lk(nodes_lock_);
            if (peer.node_id >= hb_conn_.size()) hb_conn_.resize(peer.node_id + 1);
            hb_conn_[peer.node_id] = conn;
        }

        Header h;
        h.type = HEARTBEAT;
        h.src_node = my_id_;
        h.seq = seq_.fetch_add(1);
        hb_net_.send(conn, h, payload.data(), payload.size());
    }
}

void Membership::check_missed_heartbeats() {
    uint64_t now = now_ms();
    std::unique_lock<std::shared_mutex> lk(nodes_lock_);
    for (auto& n : nodes_) {
        if (n.node_id == my_id_ || n.state == DEAD || n.state == LEFT) continue;
        if (now - n.last_hb_ms < cfg_.hb_interval_ms) continue;  // heard from them recently enough

        n.missed_hb++;
        if (n.missed_hb == cfg_.hb_suspect_after && n.state == ALIVE) {
            n.state = SUSPECT;
            LOG_WARN("node %u is now SUSPECT (%u missed heartbeats)", n.node_id, n.missed_hb);
        } else if (n.missed_hb >= cfg_.hb_dead_after && n.state == SUSPECT) {
            // Only this node's local view changes here. Officialising DEAD so every node
            // agrees (NODE_DEAD broadcast by the coordinator, releasing its locks, marking
            // its home pages LOST) is Fase 2 / E3 work (design section 7.4, PLAN.md).
            n.state = DEAD;
            LOG_ERROR("node %u is now DEAD locally (%u missed heartbeats)", n.node_id, n.missed_hb);
        }
    }
}

void Membership::heartbeat_loop() {
    uint64_t next_tick = now_ms();
    while (running_.load()) {
        if (now_ms() >= next_tick) {
            check_missed_heartbeats();
            send_heartbeats();
            next_tick = now_ms() + cfg_.hb_interval_ms;
        }
        // Short slices instead of one long sleep, so stop() takes effect promptly.
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<uint32_t>(50, cfg_.hb_interval_ms)));
    }
}
