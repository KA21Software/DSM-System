// Internal structures (design document, Annex A). Sizes are part of the design:
// the static_asserts below fail the build if padding ever changes them.
#pragma once
#include <semaphore.h>

#include <atomic>

#include "kadp_types.h"

struct PageEntry {  // 16 B, local page table
    uint8_t state;   // page_state_t
    uint8_t flags;   // ALLOCATED | HOME | LOST
    uint16_t waiters_head;  // 0xFFFF = none
    uint32_t version;
    uint32_t alloc_id;
    uint32_t alloc_order;
};

struct DirEntry {  // 24 B, directory kept by the home node
    uint16_t owner;
    uint8_t state;
    uint8_t busy;
    uint16_t queue_head;
    uint16_t queue_tail;
    uint64_t copyset;  // bitmap, hence max_nodes <= 64
    uint32_t version;
    uint16_t replica_node;
    uint16_t txn_idx;
};

struct HomeTxn {  // 24 B
    uint32_t page_id;
    uint16_t requester;
    uint8_t access;
    uint8_t phase;
    uint32_t req_seq;
    uint16_t acks_left;
    uint16_t pad;
    uint64_t started_ms;
};

struct QueuedReq {  // 12 B
    uint16_t requester;
    uint8_t access;
    uint8_t pad0;
    uint32_t req_seq;
    uint16_t next;
    uint16_t pad1;
};

// Touched from the SIGSEGV handler: no constructors that allocate, one cache line per slot.
struct alignas(64) FaultSlot {  // 64 B
    void* addr;
    uint32_t page_id;
    uint8_t access;
    std::atomic<uint8_t> in_use;
    int16_t result;
    int32_t tid;
    uint16_t next_waiter;
    uint16_t pad;
    sem_t done;
};

struct NodeInfo {  // 48 B
    uint16_t node_id;
    uint8_t state;
    uint8_t slot;
    uint32_t ipv4;
    uint16_t port_data, port_ctrl, port_hb, port_web;
    uint32_t mem_pages, free_pages;
    uint64_t joined_ms, last_hb_ms;
    uint8_t missed_hb;
    uint8_t pad[7];
};

// Deviation from the design: Annex A keeps a trailing `uint32_t pad1`, which makes the
// struct 72 B because of uint64_t alignment. Dropping it gives the intended 64 B.
struct AllocRecord {  // 64 B
    uint32_t alloc_id;
    uint16_t owner_node;
    uint16_t pad0;
    int32_t owner_pid;
    uint32_t first_page;
    uint32_t n_pages;
    uint64_t size_bytes, created_ms;
    char name[24];
};

struct NameEntry {  // 56 B
    char name[32];
    uint32_t alloc_id;
    uint32_t first_page;
    uint64_t size_bytes;
    uint16_t owner_node;
    uint8_t pad[6];
};

static_assert(sizeof(PageEntry) == 16, "PageEntry must be 16 B");
static_assert(sizeof(DirEntry) == 24, "DirEntry must be 24 B");
static_assert(sizeof(FaultSlot) == 64, "FaultSlot must be 64 B");
static_assert(sizeof(NodeInfo) == 48, "NodeInfo must be 48 B");
static_assert(sizeof(HomeTxn) == 24, "HomeTxn must be 24 B");
static_assert(sizeof(QueuedReq) == 12, "QueuedReq must be 12 B");
static_assert(sizeof(AllocRecord) == 64, "AllocRecord must be 64 B");
static_assert(sizeof(NameEntry) == 56, "NameEntry must be 56 B");
