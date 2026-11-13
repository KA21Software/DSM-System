// Shared address space of one node (design document, sections 2.5, 3.1 to 3.3).
//
// One memfd is mapped twice:
//   user view   at base_address, permissions follow the MSI state of each page; the only
//               view the application ever sees.
//   system view anywhere the kernel chooses, always read/write; the only view DSM code may
//               touch to copy page contents.
// A page that arrives from the network is copied through the system view and only then
// enabled in the user view with a single mprotect, so no thread can see it half copied.
#pragma once
#include <cstdint>
#include <mutex>
#include <vector>

#include "config.h"
#include "kadp_structs.h"
#include "kadp_types.h"

class Memory {
public:
    static constexpr uint32_t kLockStripes = 256;

    Memory() = default;
    ~Memory() { shutdown(); }
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;

    // Reserves max_nodes * node_memory bytes. Returns DSM_OK; DSM_ECONFIG if base_address is
    // already mapped (EEXIST); DSM_ENOMEM for any other system failure; DSM_EINVAL if
    // already initialised.
    int init(const Config& cfg);
    void shutdown();
    bool ready() const { return fd_ >= 0; }

    // Address arithmetic. contains() and page_of() use no locks, allocation or libc calls, so
    // they are safe to call from the SIGSEGV handler once init() has returned.
    bool contains(const void* addr) const {
        uintptr_t a = reinterpret_cast<uintptr_t>(addr);
        uintptr_t b = reinterpret_cast<uintptr_t>(user_);
        return a >= b && a - b < size_;
    }
    page_id_t page_of(const void* addr) const {
        return static_cast<page_id_t>((reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(user_)) / page_size_);
    }

    // Home of a page is computed, never looked up (section 3.2).
    node_id_t home_of(page_id_t p) const { return static_cast<node_id_t>(p / pages_per_node_); }
    uint32_t offset_of(page_id_t p) const { return p % pages_per_node_; }
    page_id_t first_page_of(node_id_t n) const { return static_cast<page_id_t>(n) * pages_per_node_; }

    uint32_t page_size() const { return page_size_; }
    uint32_t pages_per_node() const { return pages_per_node_; }
    uint32_t total_pages() const { return total_pages_; }
    bool valid_page(page_id_t p) const { return p < total_pages_; }

    // Raw addresses of a page. Application code only ever gets user_addr().
    void* user_addr(page_id_t p) const { return user_ + static_cast<size_t>(p) * page_size_; }
    void* sys_addr(page_id_t p) const { return sys_ + static_cast<size_t>(p) * page_size_; }

    // Sets the user-view permissions for an MSI state: I -> none, S -> read, M -> read/write.
    // Transitional states (IS_P, IM_P, SM_P) are not valid here; they keep the old permissions.
    int apply_prot(page_id_t p, page_state_t st);

    // Copies one page into the system view, then enables it with one mprotect.
    int install_page(page_id_t p, const void* data, page_state_t st);
    // Copies one page into the system view only, leaving the user view's permissions alone.
    // What the home does with a page it got back from an owner.
    int store_page(page_id_t p, const void* data);
    // Copies one page out of the system view (what the home or owner sends over the network).
    int copy_out(page_id_t p, void* dst) const;

    // Local page table (section 3.3). The caller holds page_lock(p) while using the entry.
    PageEntry& entry(page_id_t p) { return table_[p]; }
    std::mutex& page_lock(page_id_t p) { return locks_[p % kLockStripes]; }

    // Marks the pages of node n's range as homed here (call once when this node gets its ID).
    void activate_home_range(node_id_t n);

private:
    int fd_ = -1;
    uint8_t* user_ = nullptr;
    uint8_t* sys_ = nullptr;
    size_t size_ = 0;
    uint32_t page_size_ = 0;
    uint32_t pages_per_node_ = 0;
    uint32_t total_pages_ = 0;
    std::vector<PageEntry> table_;
    std::mutex locks_[kLockStripes];
};
