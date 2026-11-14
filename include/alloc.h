// Distributed page allocator (design section 5.5, API section 9.4). Every node is the home of
// one range and keeps the free bitmap and the allocation records of that range. A requester
// asks the homes through the control plane:
//   1. a contiguous block that fits inside one range (own range first, then the others);
//   2. otherwise a block that spans ranges: the free tail of one range plus the start of the
//      next ones. A piece that fails is rolled back, so an allocation is all or nothing.
// Because pages are contiguous in the shared address space, the returned pointer is valid on
// every node, and any node may free it.
//
// ALLOC_REQUEST deviates from table 6.3: it also carries the whole allocation's first page,
// size and name, and has ANY/TAIL modes (header flags) so the requester does not need to know
// the other homes' bitmaps. FREE_ACK carries the record that was freed.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "control.h"
#include "directory.h"
#include "kadp_structs.h"
#include "memory.h"
#include "membership.h"

class Allocator {
public:
    // One reservation inside this node's range. `rec` describes the piece (first_page and
    // n_pages are the piece's), `alloc_start` is the first page of the whole allocation.
    struct Piece {
        AllocRecord rec;
        uint32_t alloc_start;
    };

    Allocator(Memory& mem, Directory& dir, ControlPlane& cp, Membership& ms, const Config& cfg)
        : mem_(mem), dir_(dir), cp_(cp), ms_(ms), cfg_(cfg) {}

    void init(node_id_t my_id);

    // Requester side. Returns the first page, or a negative DSM_E* code.
    // alloc_id_out (optional) receives the id the pieces were recorded under.
    int64_t allocate(size_t size, const std::string& name, uint32_t* alloc_id_out = nullptr);
    // Frees the allocation that starts at first_page. alloc_id_out receives its id (names are
    // removed by the caller, which knows the names table).
    int release(page_id_t first_page, uint32_t& alloc_id_out);

    // Home side: ALLOC_REQUEST and FREE, on the control listener. Never waits for the network.
    void on_message(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload);

    uint32_t free_pages();
    std::vector<Piece> pieces();  // copy, for the console
    bool piece_of(page_id_t page, Piece& out);

private:
    struct Request {
        uint32_t alloc_id = 0, first_page = 0, n_pages = 0, alloc_start = 0;
        int32_t pid = 0;
        uint64_t size = 0;
        char name[24] = {};
    };
    struct FreeJob;  // a FREE waiting for the directory to clean its pages

    void on_alloc_request(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload);
    void on_free(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload);
    int reserve(const Request& r, uint16_t flags, node_id_t requester, uint32_t& first, uint32_t& count);
    bool alive(node_id_t n);
    int ask_home(node_id_t home, const Request& r, uint16_t flags, uint32_t& first, uint32_t& count);
    void give_back(const std::vector<std::pair<uint32_t, uint32_t>>& pieces, uint32_t alloc_id);
    int free_piece(node_id_t home, uint32_t alloc_id, uint32_t first, uint32_t n, ControlPlane::Reply& out);

    Memory& mem_;
    Directory& dir_;
    ControlPlane& cp_;
    Membership& ms_;
    const Config& cfg_;
    node_id_t my_id_ = NODE_NONE;
    std::atomic<uint32_t> next_id_{1};

    std::mutex lock_;  // alloc_lock of the design's lock order
    std::vector<uint8_t> used_;
    std::vector<Piece> pieces_;
};
