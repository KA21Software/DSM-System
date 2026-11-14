#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // REG_ERR (ucontext_t.uc_mcontext.gregs) is a glibc/x86-64 extension
#endif

#include "pager.h"

#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

#include "log.h"
#include "stats.h"

namespace {

// sigaction() is process-wide, and the handler is a plain C function with no `this`: it
// reaches the one active Pager through this pointer instead. Set/cleared by start()/stop(),
// always before the handler could possibly be invoked (see the ordering notes there).
std::atomic<Pager*> g_pager{nullptr};

void segv_trampoline(int sig, siginfo_t* info, void* ucontext) {
    Pager* p = g_pager.load();
    if (p) p->handle_segv(sig, info, ucontext);
}

uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint64_t now_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

bool is_transitional(uint8_t state) { return state == IS_P || state == IM_P || state == SM_P; }

bool access_satisfied(uint8_t state, access_t access) {
    return access == ACC_WRITE ? state == M : (state == S || state == M);
}

}  // namespace

Pager::Pager(Memory& mem, Network& data_net) : mem_(mem), data_net_(data_net) {}

Pager::~Pager() { stop(); }

int Pager::start() {
    Pager* expected = nullptr;
    if (!g_pager.compare_exchange_strong(expected, this)) return DSM_EINVAL;

    for (int i = 0; i < kQueueSize; i++) {
        faults_[i].in_use.store(0);
        dispatched_[i].store(0);
        fault_started_ms_[i] = 0;
        if (sem_init(&faults_[i].done, 0, 0) != 0) {
            LOG_ERROR("sem_init(faults_[%d].done) failed: %s", i, strerror(errno));
            for (int j = 0; j < i; j++) sem_destroy(&faults_[j].done);
            g_pager.store(nullptr);
            return DSM_EINVAL;
        }
    }
    if (sem_init(&pager_sem_, 0, 0) != 0) {
        LOG_ERROR("sem_init(pager_sem_) failed: %s", strerror(errno));
        for (int i = 0; i < kQueueSize; i++) sem_destroy(&faults_[i].done);
        g_pager.store(nullptr);
        return DSM_EINVAL;
    }

    struct sigaction sa{};
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = &segv_trampoline;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &prev_sigaction_) != 0) {
        LOG_ERROR("sigaction(SIGSEGV) failed: %s", strerror(errno));
        sem_destroy(&pager_sem_);
        for (int i = 0; i < kQueueSize; i++) sem_destroy(&faults_[i].done);
        g_pager.store(nullptr);
        return DSM_EINVAL;
    }

    running_ = true;
    thread_ = std::thread(&Pager::pager_thread_loop, this);
    LOG_INFO("pager started (queue size %d, request_timeout_ms %u)", kQueueSize, request_timeout_ms_);
    return DSM_OK;
}

void Pager::stop() {
    if (g_pager.load() != this) return;  // never started, or already stopped

    sigaction(SIGSEGV, &prev_sigaction_, nullptr);
    g_pager.store(nullptr);  // no fault reaching segv_trampoline can find us from here on

    running_ = false;
    if (thread_.joinable()) thread_.join();

    sem_destroy(&pager_sem_);
    for (int i = 0; i < kQueueSize; i++) sem_destroy(&faults_[i].done);
}

// ---- Async-signal-safe section: handle_segv and acquire_fault_slot only. Nothing here may
// call malloc/new, printf/cout, pthread_mutex_lock, or any std:: container that allocates.

int Pager::acquire_fault_slot() {
    for (int i = 0; i < kQueueSize; i++) {
        uint8_t expected = 0;
        if (faults_[i].in_use.compare_exchange_strong(expected, 1)) return i;
    }
    return -1;
}

void Pager::handle_segv(int sig, siginfo_t* info, void* ucontext) {
    if (!mem_.contains(info->si_addr)) {
        // Outside our reserved range: a real bug in the application, not a page we manage.
        // Restore the default action and re-raise so the normal segfault handling applies.
        struct sigaction def{};
        def.sa_handler = SIG_DFL;
        sigemptyset(&def.sa_mask);
        sigaction(sig, &def, nullptr);
        raise(sig);
        return;
    }

    page_id_t page = mem_.page_of(info->si_addr);
    ucontext_t* uc = static_cast<ucontext_t*>(ucontext);
    bool is_write = (uc->uc_mcontext.gregs[REG_ERR] & 0x2) != 0;  // bit 1 of the error code

    int slot = acquire_fault_slot();
    if (slot < 0) {
        // The preallocated queue (64 slots) is exhausted. Recovering would need a bigger
        // queue, which needs malloc: not an option here. Documented limit, see design 5.4.
        const char msg[] = "pager: fault queue full, aborting\n";
        write(STDERR_FILENO, msg, sizeof msg - 1);
        abort();
    }

    FaultSlot& fs = faults_[slot];
    fs.addr = info->si_addr;
    fs.page_id = page;
    fs.access = is_write ? ACC_WRITE : ACC_READ;
    fs.tid = static_cast<int32_t>(syscall(SYS_gettid));
    fs.result = 0;

    sem_post(&pager_sem_);
    while (sem_wait(&fs.done) != 0 && errno == EINTR) {}

    // Read fs.result before releasing the slot: sem_wait's return establishes a
    // happens-before with the pager/listener thread's write to it, but only up to here.
    int16_t result = fs.result;
    dispatched_[slot].store(0);
    fs.in_use.store(0);

    if (result != 0) {
        // Fixed strings only: nothing but write() is allowed here.
        const char generic[] = "pager: fault could not be resolved, aborting\n";
        const char down[] = "pager: DSM_ENODEDOWN: the node that holds this page is gone, aborting\n";
        const char notalloc[] = "pager: DSM_ENOTALLOC: access to a page that is not allocated, aborting\n";
        const char timeout[] = "pager: DSM_ETIMEOUT: no answer for this page, aborting\n";
        if (result == DSM_ENODEDOWN) write(STDERR_FILENO, down, sizeof down - 1);
        else if (result == DSM_ENOTALLOC) write(STDERR_FILENO, notalloc, sizeof notalloc - 1);
        else if (result == DSM_ETIMEOUT) write(STDERR_FILENO, timeout, sizeof timeout - 1);
        else write(STDERR_FILENO, generic, sizeof generic - 1);
        abort();
    }
    // Returning retries the faulting instruction, which now has the right permission.
}

// ---- Everything below runs on the pager thread or the Network listener thread: ordinary
// mutexes and std:: containers are fine here.

void Pager::pager_thread_loop() {
    while (running_.load()) {
        timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100'000'000;  // 100 ms slices: bounds how long stop() takes to notice
        if (ts.tv_nsec >= 1'000'000'000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1'000'000'000;
        }
        sem_timedwait(&pager_sem_, &ts);  // either a fresh fault or just a tick; either way, scan

        for (int i = 0; i < kQueueSize; i++) {
            uint8_t expected = 0;
            if (faults_[i].in_use.load() && dispatched_[i].compare_exchange_strong(expected, 1)) {
                fault_started_ms_[i] = now_ms();
                fault_started_us_[i] = now_us();
                handle_fault(i);
            }
        }
        sweep_timeouts();
    }
}

void Pager::handle_fault(int slot_idx) {
    FaultSlot& fs = faults_[slot_idx];
    page_id_t page = fs.page_id;
    access_t access = static_cast<access_t>(fs.access);
    node_id_t home = NODE_NONE;
    bool must_send = false;
    counters().add(access == ACC_WRITE ? counters().faults_write : counters().faults_read);

    {
        std::lock_guard<std::mutex> lk(mem_.page_lock(page));
        PageEntry& entry = mem_.entry(page);

        if (access_satisfied(entry.state, access)) {
            resolve_slot(slot_idx, DSM_OK);
            return;
        }
        if (entry.flags & PG_LOST) {  // its home died: there is nobody to ask
            resolve_slot(slot_idx, DSM_ENODEDOWN);
            return;
        }

        fs.next_waiter = entry.waiters_head;
        entry.waiters_head = static_cast<uint16_t>(slot_idx);

        if (!is_transitional(entry.state)) {
            // One request per node per page, no matter how many local threads fault on it
            // (design section 4.4): only the first waiter for a fresh I or S state sends.
            entry.state = (access == ACC_WRITE) ? (entry.state == S ? SM_P : IM_P) : IS_P;
            home = mem_.home_of(page);
            must_send = true;
        }
    }

    // Sent outside the lock: connecting to the home can block for a while (connect_to()'s
    // timeout), and nothing about that wait should hold up unrelated pages in the same
    // lock stripe (design's "no network waits while holding a lock" applies to replies, but
    // minimising hold time is good practice regardless).
    if (must_send && !send_page_request(home, page, access)) fail_page(page, DSM_ENODEDOWN);
}

// The request never left this node (no route to the home): wake everyone waiting on the page.
void Pager::fail_page(page_id_t page, int status) {
    std::lock_guard<std::mutex> lk(mem_.page_lock(page));
    PageEntry& entry = mem_.entry(page);
    if (!is_transitional(entry.state)) return;
    entry.state = I;
    uint16_t w = entry.waiters_head;
    entry.waiters_head = NO_WAITER;
    while (w != NO_WAITER) {
        uint16_t next = faults_[w].next_waiter;
        resolve_slot(w, status);
        w = next;
    }
}

void Pager::on_node_down(node_id_t node) {
    page_id_t first = mem_.first_page_of(node);
    for (page_id_t p = first; p < first + mem_.pages_per_node() && p < mem_.total_pages(); p++) {
        std::lock_guard<std::mutex> lk(mem_.page_lock(p));
        PageEntry& entry = mem_.entry(p);
        entry.flags |= PG_LOST;
        if (entry.state == S || entry.state == M) {
            mem_.apply_prot(p, I);
            entry.state = I;
        } else if (is_transitional(entry.state)) {
            entry.state = I;
            uint16_t w = entry.waiters_head;
            entry.waiters_head = NO_WAITER;
            while (w != NO_WAITER) {
                uint16_t next = faults_[w].next_waiter;
                resolve_slot(w, DSM_ENODEDOWN);
                w = next;
            }
        }
    }
}

void Pager::resolve_slot(int slot, int status) {
    if (status == DSM_OK && fault_started_us_[slot]) counters().note_fault_latency(now_us() - fault_started_us_[slot]);
    faults_[slot].result = static_cast<int16_t>(status);
    sem_post(&faults_[slot].done);
}

void Pager::unlink_waiter(PageEntry& entry, uint16_t slot) {
    uint16_t* link = &entry.waiters_head;
    while (*link != NO_WAITER) {
        if (*link == slot) {
            *link = faults_[slot].next_waiter;
            return;
        }
        link = &faults_[*link].next_waiter;
    }
}

void Pager::sweep_timeouts() {
    uint64_t now = now_ms();
    for (int i = 0; i < kQueueSize; i++) {
        if (!faults_[i].in_use.load() || !dispatched_[i].load()) continue;
        if (now - fault_started_ms_[i] < request_timeout_ms_) continue;

        page_id_t page = faults_[i].page_id;
        std::lock_guard<std::mutex> lk(mem_.page_lock(page));
        PageEntry& entry = mem_.entry(page);
        unlink_waiter(entry, static_cast<uint16_t>(i));
        // Nobody left waiting: let a future fault retry with a fresh PAGE_REQUEST instead of
        // staying stuck in a transitional state that nothing will ever resolve (E2 scope has
        // no automatic retry/backoff; see request_timeout_ms_ in pager.h).
        if (entry.waiters_head == NO_WAITER) entry.state = I;
        resolve_slot(i, DSM_ETIMEOUT);
    }
}

std::shared_ptr<Connection> Pager::data_connection(node_id_t home) {
    {
        std::lock_guard<std::mutex> lk(conn_lock_);
        if (home < data_conn_.size() && data_conn_[home] && !data_conn_[home]->closed.load()) return data_conn_[home];
    }
    std::string ip;
    uint16_t port;
    if (!home_lookup_ || !home_lookup_(home, ip, port)) return nullptr;

    auto conn = data_net_.connect_to(ip, port, request_timeout_ms_);
    if (!conn) return nullptr;
    std::lock_guard<std::mutex> lk(conn_lock_);
    if (static_cast<size_t>(home) >= data_conn_.size()) data_conn_.resize(home + 1);
    data_conn_[home] = conn;
    return conn;
}

bool Pager::send_page_request(node_id_t home, page_id_t page, access_t access) {
    auto conn = data_connection(home);
    if (!conn) return false;  // no route to the home: it is down or unreachable

    Header h;
    h.type = PAGE_REQUEST;
    h.page_id = page;
    h.flags = (access == ACC_WRITE) ? F_ACCESS_WRITE : 0;
    h.src_node = my_node_id_;
    return data_net_.send(conn, h, nullptr, 0) == DSM_OK;
}

void Pager::complete_grant(page_id_t page, version_t version, page_state_t granted, const uint8_t* data) {
    std::lock_guard<std::mutex> lk(mem_.page_lock(page));
    PageEntry& entry = mem_.entry(page);
    if (version < entry.version) return;  // stale/duplicate reply (design section 6.2): discard

    if (data) mem_.install_page(page, data, granted);  // PAGE_DATA: copy bytes, then one mprotect
    else mem_.apply_prot(page, granted);                // UPGRADE_ACK: the S copy's bytes already fit
    entry.state = granted;
    entry.version = version;

    uint16_t w = entry.waiters_head;
    entry.waiters_head = NO_WAITER;
    while (w != NO_WAITER) {
        uint16_t next = faults_[w].next_waiter;
        resolve_slot(w, DSM_OK);
        w = next;
    }
}

void Pager::on_page_data(const Header& h, const uint8_t* payload) {
    counters().add(counters().pages_received);
    page_state_t granted = (h.flags & F_GRANT_M) ? M : S;
    complete_grant(h.page_id, h.page_version, granted, payload);
}

void Pager::on_upgrade_ack(const Header& h) { complete_grant(h.page_id, h.page_version, M, nullptr); }

void Pager::on_invalidate(const std::shared_ptr<Connection>& conn, const Header& h) {
    page_id_t page = h.page_id;
    counters().add(counters().invalidations_received);
    {
        std::lock_guard<std::mutex> lk(mem_.page_lock(page));
        PageEntry& entry = mem_.entry(page);
        if (entry.state == SM_P) {
            // We lost our S copy while waiting for an upgrade (design section 3.5): the home
            // will answer our still-pending request with PAGE_DATA(M) instead of UPGRADE_ACK.
            entry.state = IM_P;
        } else if (entry.state == S) {
            mem_.apply_prot(page, I);
            entry.state = I;
        }
        // Any other state: a stale/duplicate INVALIDATE for a copy we no longer have as S.
    }

    Header ack;
    ack.type = INVALIDATE_ACK;
    ack.page_id = page;
    ack.src_node = my_node_id_;
    data_net_.send(conn, ack, nullptr, 0);
}

void Pager::on_fetch(const std::shared_ptr<Connection>& conn, const Header& h, bool invalidate) {
    page_id_t page = h.page_id;
    std::vector<uint8_t> data;
    version_t version = 0;
    bool have_data = false;

    {
        std::lock_guard<std::mutex> lk(mem_.page_lock(page));
        PageEntry& entry = mem_.entry(page);
        if (entry.state == M) {
            // Revoke write access first, copy after: a store that slipped in between the
            // other order would be lost. The system view still sees the bytes either way.
            page_state_t next = invalidate ? I : S;
            mem_.apply_prot(page, next);
            data.resize(mem_.page_size());
            mem_.copy_out(page, data.data());
            entry.state = next;
            version = entry.version;
            have_data = true;
        }
        // Not in M: a stale/duplicate FETCH for a page we no longer own. The home's directory
        // already has (or will get) the data from whoever it actually granted M to.
    }

    if (!have_data) return;
    Header reply;
    reply.type = FETCH_REPLY;
    reply.page_id = page;
    reply.page_version = version;
    reply.src_node = my_node_id_;
    data_net_.send(conn, reply, data.data(), data.size());
}

void Pager::on_coherence_message(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload) {
    switch (h.type) {
        case PAGE_DATA:
            on_page_data(h, payload);
            break;
        case UPGRADE_ACK:
            on_upgrade_ack(h);
            break;
        case INVALIDATE:
            on_invalidate(conn, h);
            break;
        case FETCH:
            on_fetch(conn, h, false);
            break;
        case FETCH_INVALIDATE:
            on_fetch(conn, h, true);
            break;
        default:
            // PAGE_REQUEST, FETCH_REPLY, INVALIDATE_ACK: home-side concerns (directory.cpp),
            // not this node acting as a requester or copy-holder.
            break;
    }
}

void Pager::on_error(const std::shared_ptr<Connection>&, const Header& h, const uint8_t*) {
    // Matched by page_id, not seq: at most one PAGE_REQUEST is ever in flight per page (see
    // handle_fault), so page_id alone identifies it. directory.cpp must echo the request's
    // page_id back in an ERROR reply for this to work, which is this module's only
    // expectation of it.
    page_id_t page = h.page_id;
    std::lock_guard<std::mutex> lk(mem_.page_lock(page));
    PageEntry& entry = mem_.entry(page);
    if (!is_transitional(entry.state)) return;  // nothing of ours is waiting on this page

    entry.state = I;  // the request failed; exactly as unavailable as before it was sent
    uint16_t w = entry.waiters_head;
    entry.waiters_head = NO_WAITER;
    while (w != NO_WAITER) {
        uint16_t next = faults_[w].next_waiter;
        resolve_slot(w, h.status);
        w = next;
    }
}
