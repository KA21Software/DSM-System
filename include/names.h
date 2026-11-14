// Table of named allocations (design sections 5.5 and 9.4). The coordinator is the single
// writer: it decides who registers a name first and announces every change; every node keeps
// a full replica, so dsm_lookup is local and a node that joins later gets the table in
// JOIN_ACK. NAME_REGISTER, NAME_REMOVE and NAME_ANNOUNCE (op byte + NameEntry) deviate a
// little from table 6.3: NAME_REMOVE is new and the announce says whether it adds or removes.
#pragma once
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "control.h"
#include "kadp_structs.h"
#include "membership.h"

class Names {
public:
    Names(ControlPlane& cp, Membership& ms, const Config& cfg) : cp_(cp), ms_(ms), cfg_(cfg) {}

    // Asks the coordinator to register `e`. DSM_OK; DSM_EEXIST with `e` replaced by the entry
    // that already holds the name (so the caller can use it if the size matches).
    int register_name(NameEntry& e);
    // Local replica first, then the coordinator (the announce may still be on its way).
    int lookup(const std::string& name, NameEntry& out);
    int remove_by_alloc(uint32_t alloc_id);

    // Control listener: NAME_REGISTER, NAME_LOOKUP, NAME_REMOVE (coordinator) and NAME_ANNOUNCE (everyone).
    void on_message(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload);

    std::vector<NameEntry> all();
    // JOIN_ACK support.
    uint16_t export_all(std::vector<uint8_t>& out);
    void import_all(const uint8_t* data, size_t len, uint16_t count);

    static constexpr size_t kMaxNames = 128;

private:
    void put(const NameEntry& e);
    bool find(const std::string& name, NameEntry& out);
    void announce(uint8_t op, const NameEntry& e);

    ControlPlane& cp_;
    Membership& ms_;
    const Config& cfg_;
    std::mutex lock_;  // names_lock of the design's lock order
    std::map<std::string, NameEntry> table_;
};

void write_name_entry(ByteWriter& w, const NameEntry& e);
bool read_name_entry(ByteReader& r, NameEntry& e);
