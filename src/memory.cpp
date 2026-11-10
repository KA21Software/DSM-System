#include "memory.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "log.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000  // kernel >= 4.17; older headers do not define it
#endif

int Memory::init(const Config& cfg) {
    if (fd_ >= 0) return DSM_EINVAL;

    size_ = static_cast<size_t>(cfg.max_nodes) * cfg.node_memory;
    page_size_ = cfg.page_size;
    pages_per_node_ = cfg.pages_per_node();
    total_pages_ = cfg.max_nodes * pages_per_node_;

    // CLOEXEC keeps the descriptor from leaking into any process the app later execs.
    fd_ = memfd_create("kadp_dsm", MFD_CLOEXEC);
    if (fd_ < 0) {
        LOG_ERROR("memfd_create failed: %s", strerror(errno));
        return DSM_ENOMEM;
    }
    if (ftruncate(fd_, static_cast<off_t>(size_)) != 0) {
        LOG_ERROR("ftruncate(%zu) failed: %s", size_, strerror(errno));
        shutdown();
        return DSM_ENOMEM;
    }

    void* want = reinterpret_cast<void*>(static_cast<uintptr_t>(cfg.base_address));
    void* u = mmap(want, size_, PROT_NONE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd_, 0);
    if (u == MAP_FAILED || u != want) {
        // Kernels before 4.17 treat NOREPLACE as a plain hint and may return another address.
        int e = u == MAP_FAILED ? errno : EEXIST;
        if (u != MAP_FAILED) munmap(u, size_);
        LOG_ERROR("cannot map user view at 0x%llx: %s", static_cast<unsigned long long>(cfg.base_address), strerror(e));
        shutdown();
        return e == EEXIST ? DSM_ECONFIG : DSM_ENOMEM;
    }
    user_ = static_cast<uint8_t*>(u);

    void* s = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (s == MAP_FAILED) {
        LOG_ERROR("cannot map system view: %s", strerror(errno));
        shutdown();
        return DSM_ENOMEM;
    }
    sys_ = static_cast<uint8_t*>(s);

    PageEntry blank{};
    blank.state = I;
    blank.waiters_head = NO_WAITER;
    table_.assign(total_pages_, blank);

    LOG_INFO("memory ready: user view %p, system view %p, %u pages (%u per node), page_size %u",
             static_cast<void*>(user_), static_cast<void*>(sys_), total_pages_, pages_per_node_, page_size_);
    return DSM_OK;
}

void Memory::shutdown() {
    if (sys_) munmap(sys_, size_);
    if (user_) munmap(user_, size_);
    if (fd_ >= 0) close(fd_);
    sys_ = user_ = nullptr;
    fd_ = -1;
    size_ = 0;
    table_.clear();
}

int Memory::apply_prot(page_id_t p, page_state_t st) {
    if (!ready() || !valid_page(p)) return DSM_EINVAL;
    int prot;
    switch (st) {
        case I: prot = PROT_NONE; break;
        case S: prot = PROT_READ; break;
        case M: prot = PROT_READ | PROT_WRITE; break;
        default: return DSM_EINVAL;
    }
    if (mprotect(user_addr(p), page_size_, prot) != 0) {
        LOG_ERROR("mprotect page %u failed: %s", p, strerror(errno));
        return DSM_ENOMEM;
    }
    return DSM_OK;
}

int Memory::install_page(page_id_t p, const void* data, page_state_t st) {
    if (!ready() || !valid_page(p) || !data) return DSM_EINVAL;
    if (st != S && st != M) return DSM_EINVAL;  // installing data only makes sense for S or M
    memcpy(sys_addr(p), data, page_size_);
    return apply_prot(p, st);
}

int Memory::copy_out(page_id_t p, void* dst) const {
    if (!ready() || !valid_page(p) || !dst) return DSM_EINVAL;
    memcpy(dst, sys_addr(p), page_size_);
    return DSM_OK;
}

void Memory::activate_home_range(node_id_t n) {
    page_id_t first = first_page_of(n);
    for (page_id_t p = first; p < first + pages_per_node_ && p < total_pages_; p++) {
        std::lock_guard<std::mutex> lk(page_lock(p));
        table_[p].flags |= PG_HOME;
    }
}
