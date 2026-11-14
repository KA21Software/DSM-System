#include "names.h"

#include <unistd.h>

#include <cstring>

#include "log.h"

void write_name_entry(ByteWriter& w, const NameEntry& e) {
    w.bytes(e.name, sizeof e.name);
    w.u32(e.alloc_id);
    w.u32(e.first_page);
    w.u64(e.size_bytes);
    w.u16(e.owner_node);
    const uint8_t pad[6] = {};
    w.bytes(pad, sizeof pad);
}

bool read_name_entry(ByteReader& r, NameEntry& e) {
    memset(&e, 0, sizeof e);
    r.bytes(e.name, sizeof e.name);
    e.name[sizeof e.name - 1] = '\0';
    e.alloc_id = r.u32();
    e.first_page = r.u32();
    e.size_bytes = r.u64();
    e.owner_node = r.u16();
    uint8_t pad[6];
    r.bytes(pad, sizeof pad);
    return r.ok();
}

void Names::put(const NameEntry& e) {
    std::lock_guard<std::mutex> lk(lock_);
    table_[e.name] = e;
}

bool Names::find(const std::string& name, NameEntry& out) {
    std::lock_guard<std::mutex> lk(lock_);
    auto it = table_.find(name);
    if (it == table_.end()) return false;
    out = it->second;
    return true;
}

std::vector<NameEntry> Names::all() {
    std::lock_guard<std::mutex> lk(lock_);
    std::vector<NameEntry> v;
    for (auto& kv : table_) v.push_back(kv.second);
    return v;
}

uint16_t Names::export_all(std::vector<uint8_t>& out) {
    ByteWriter w(out);
    std::lock_guard<std::mutex> lk(lock_);
    for (auto& kv : table_) write_name_entry(w, kv.second);
    return static_cast<uint16_t>(table_.size());
}

void Names::import_all(const uint8_t* data, size_t len, uint16_t count) {
    ByteReader r(data, len);
    for (uint16_t i = 0; i < count; i++) {
        NameEntry e;
        if (!read_name_entry(r, e)) break;
        put(e);
    }
}

// Coordinator only: tells every other node about a change (op 0 = add, 1 = remove).
void Names::announce(uint8_t op, const NameEntry& e) {
    std::vector<uint8_t> payload;
    ByteWriter w(payload);
    w.u8(op);
    write_name_entry(w, e);
    Header h;
    h.type = NAME_ANNOUNCE;
    cp_.broadcast(h, payload.data(), payload.size(), false);
}

int Names::register_name(NameEntry& e) {
    for (int attempt = 0; attempt < 3; attempt++) {
        node_id_t coord = ms_.coordinator_id();
        if (coord == NODE_NONE) return DSM_ENODEDOWN;
        std::vector<uint8_t> payload;
        ByteWriter w(payload);
        write_name_entry(w, e);
        Header h;
        h.type = NAME_REGISTER;
        ControlPlane::Reply rep;
        int rc = cp_.call(coord, h, payload.data(), payload.size(), rep, cfg_.request_timeout_ms);
        if (rc == DSM_OK) {
            put(e);
            return DSM_OK;
        }
        if (rc == DSM_EEXIST) {
            ByteReader r(rep.payload.data(), rep.payload.size());
            NameEntry existing;
            if (read_name_entry(r, existing)) {
                e = existing;
                put(existing);
            }
            return DSM_EEXIST;
        }
        if (rc != DSM_ENODEDOWN) return rc;
        usleep(200 * 1000);  // the coordinator changed under us: look again
    }
    return DSM_ENODEDOWN;
}

int Names::lookup(const std::string& name, NameEntry& out) {
    if (find(name, out)) return DSM_OK;
    node_id_t coord = ms_.coordinator_id();
    if (coord == NODE_NONE) return DSM_ENODEDOWN;
    std::vector<uint8_t> payload;
    ByteWriter w(payload);
    char buf[32] = {};
    strncpy(buf, name.c_str(), sizeof buf - 1);
    w.bytes(buf, sizeof buf);
    Header h;
    h.type = NAME_LOOKUP;
    ControlPlane::Reply rep;
    int rc = cp_.call(coord, h, payload.data(), payload.size(), rep, cfg_.request_timeout_ms);
    if (rc != DSM_OK) return rc;
    ByteReader r(rep.payload.data(), rep.payload.size());
    if (!read_name_entry(r, out)) return DSM_EPROTO;
    put(out);
    return DSM_OK;
}

int Names::remove_by_alloc(uint32_t alloc_id) {
    NameEntry e{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(lock_);
        for (auto& kv : table_)
            if (kv.second.alloc_id == alloc_id) {
                e = kv.second;
                found = true;
            }
    }
    if (!found) return DSM_OK;  // it never had a name
    node_id_t coord = ms_.coordinator_id();
    if (coord == NODE_NONE) return DSM_ENODEDOWN;
    std::vector<uint8_t> payload;
    ByteWriter w(payload);
    write_name_entry(w, e);
    Header h;
    h.type = NAME_REMOVE;
    ControlPlane::Reply rep;
    int rc = cp_.call(coord, h, payload.data(), payload.size(), rep, cfg_.request_timeout_ms);
    if (rc == DSM_OK || rc == DSM_ENOTFOUND) {
        std::lock_guard<std::mutex> lk(lock_);
        table_.erase(e.name);
        return DSM_OK;
    }
    return rc;
}

void Names::on_message(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload) {
    ByteReader r(payload, h.payload_len);
    if (h.type == NAME_ANNOUNCE) {
        uint8_t op = r.u8();
        NameEntry e;
        if (!read_name_entry(r, e)) return;
        if (op == 0) {
            put(e);
        } else {
            std::lock_guard<std::mutex> lk(lock_);
            table_.erase(e.name);
        }
        return;
    }

    // The rest is coordinator work. A node that is not (or no longer) the coordinator says so
    // and the caller looks again.
    if (!ms_.is_coordinator()) {
        cp_.reply(conn, h, h.type == NAME_LOOKUP ? NAME_REPLY : NAME_REPLY, DSM_ENODEDOWN);
        return;
    }
    NameEntry e;
    if (h.type == NAME_LOOKUP) {
        memset(&e, 0, sizeof e);
        r.bytes(e.name, sizeof e.name);
        e.name[sizeof e.name - 1] = '\0';
        if (!r.ok()) return cp_.reply(conn, h, NAME_REPLY, DSM_EPROTO);
        NameEntry found;
        if (!find(e.name, found)) return cp_.reply(conn, h, NAME_REPLY, DSM_ENOTFOUND);
        std::vector<uint8_t> out;
        ByteWriter w(out);
        write_name_entry(w, found);
        return cp_.reply(conn, h, NAME_REPLY, DSM_OK, out.data(), out.size());
    }
    if (!read_name_entry(r, e)) return cp_.reply(conn, h, NAME_REPLY, DSM_EPROTO);

    if (h.type == NAME_REGISTER) {
        NameEntry existing;
        std::vector<uint8_t> out;
        ByteWriter w(out);
        bool exists, full = false;
        {
            std::lock_guard<std::mutex> lk(lock_);
            auto it = table_.find(e.name);
            exists = it != table_.end();
            if (exists) existing = it->second;
            else if (table_.size() >= kMaxNames) full = true;
            else table_[e.name] = e;
        }
        if (exists) {
            write_name_entry(w, existing);
            return cp_.reply(conn, h, NAME_REPLY, DSM_EEXIST, out.data(), out.size());
        }
        if (full) return cp_.reply(conn, h, NAME_REPLY, DSM_ENOMEM);
        announce(0, e);
        cp_.reply(conn, h, NAME_REPLY, DSM_OK);
    } else if (h.type == NAME_REMOVE) {
        bool had;
        {
            std::lock_guard<std::mutex> lk(lock_);
            had = table_.erase(e.name) > 0;
        }
        if (had) announce(1, e);
        cp_.reply(conn, h, NAME_REPLY, had ? DSM_OK : DSM_ENOTFOUND);
    }
}
