// Common types shared by every module (design document, Annex C).
#pragma once
#include <cstddef>
#include <cstdint>

#include "dsm.h"

using node_id_t = uint16_t;
using page_id_t = uint32_t;
using alloc_id_t = uint32_t;
using seq_t = uint32_t;
using version_t = uint32_t;

enum page_state_t : uint8_t { I = 0, S = 1, M = 2, IS_P = 3, IM_P = 4, SM_P = 5 };
enum dir_state_t : uint8_t { D_SHARED = 0, D_MODIFIED = 1 };
enum access_t : uint8_t { ACC_READ = 0, ACC_WRITE = 1 };
enum node_state_t : uint8_t { ALIVE = 0, SUSPECT = 1, DEAD = 2, LEFT = 3 };

constexpr uint32_t KADP_MAGIC = 0x4B414450;  // 'KADP'
constexpr uint8_t KADP_VERSION = 1;
constexpr size_t KADP_HDR_LEN = 40;

enum kadp_flags : uint16_t {
    F_IS_REPLY = 1, F_ACCESS_WRITE = 2, F_GRANT_M = 4, F_FORWARDED = 8, F_BROADCAST = 16,
    F_ALLOC_ANY = 32,   // ALLOC_REQUEST: the home picks the pages (first fit in its range)
    F_ALLOC_TAIL = 64   // ALLOC_REQUEST: the home gives the free tail of its range (spanning allocs)
};

// The high nibble is the message class (type >> 4), used by the listener to route.
enum kadp_type : uint8_t {
    PAGE_REQUEST = 0x10, PAGE_DATA = 0x11, UPGRADE_ACK = 0x12, INVALIDATE = 0x13,
    INVALIDATE_ACK = 0x14, FETCH = 0x15, FETCH_INVALIDATE = 0x16, FETCH_REPLY = 0x17,
    ALLOC_REQUEST = 0x20, ALLOC_REPLY = 0x21, FREE = 0x22, FREE_ACK = 0x23,
    NAME_REGISTER = 0x24, NAME_LOOKUP = 0x25, NAME_REPLY = 0x26, NAME_ANNOUNCE = 0x27,
    NAME_REMOVE = 0x28,  // not in the E1 tables: dsm_free needs to drop a name at the coordinator
    JOIN = 0x30, JOIN_ACK = 0x31, NODE_ANNOUNCE = 0x32, HEARTBEAT = 0x33, LEAVE = 0x34,
    NODE_DEAD = 0x35,
    STATS_REQUEST = 0x40, STATS_REPLY = 0x41, PAGE_TABLE_REQUEST = 0x42,
    PAGE_TABLE_REPLY = 0x43, ALLOCS_REQUEST = 0x44, ALLOCS_REPLY = 0x45,
    LOCK_ACQUIRE = 0x50, LOCK_GRANT = 0x51, LOCK_RELEASE = 0x52,
    BARRIER_ENTER = 0x53, BARRIER_RELEASE = 0x54,
    KADP_ERROR = 0xF0
};

// DSM_E* error codes (design table 6.6) live in dsm.h: the protocol (header.status) and the
// public API share them.
