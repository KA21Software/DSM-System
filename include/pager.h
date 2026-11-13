// Demand paging: the SIGSEGV handler, the preallocated fault queue, and the pager thread
// that turns a fault into a PAGE_REQUEST and, later, a resolved page (design section 3.7).
//
// Scope: this module only ever plays the requester / copy-holder role for a page. It always
// asks the page's home over the network, even when that home is this very node — the
// optimisation of answering a local request without a round trip belongs to directory.cpp
// (not built yet), which is the one that actually decides directory state. Once it exists,
// PAGE_REQUEST and FETCH_REPLY (the home-side messages) are handled there; a single combined
// class-1 handler then calls both this module's on_coherence_message() and directory's,
// since Network keeps one handler per message class, not per type.
#pragma once
#include <semaphore.h>
#include <signal.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kadp_structs.h"
#include "kadp_types.h"
#include "memory.h"
#include "network.h"

class Pager {
public:
    static constexpr int kQueueSize = 64;  // design section 5.4: FaultSlot[64], preallocated

    // node_id -> (ip, data-channel port) for a page's home. False if unknown; membership
    // owns the node table, so whoever wires the node together supplies this.
    using HomeLookup = std::function<bool(node_id_t home, std::string& ip, uint16_t& port)>;

    Pager(Memory& mem, Network& data_net);
    ~Pager();
    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

    void set_my_node_id(node_id_t id) { my_node_id_ = id; }
    void set_home_lookup(HomeLookup fn) { home_lookup_ = std::move(fn); }
    void set_request_timeout_ms(uint32_t ms) { request_timeout_ms_ = ms; }

    // Installs the SIGSEGV handler and starts the pager thread. Only one Pager may be active
    // per process: sigaction() is process-wide, so the handler reaches back through a single
    // global pointer (see pager.cpp) rather than a `this`. DSM_EINVAL if one is already
    // running or if sigaction()/sem_init() fails.
    int start();
    void stop();

    // Handler for PAGE_DATA, UPGRADE_ACK, INVALIDATE, FETCH and FETCH_INVALIDATE — everything
    // in msg_class(PAGE_REQUEST) == 1 except PAGE_REQUEST and FETCH_REPLY themselves, which
    // are answered by directory.cpp (see the file comment above). Register on data_net's
    // class 1.
    void on_coherence_message(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload);
    // Handler for msg_class(KADP_ERROR): a rejected PAGE_REQUEST. Register on data_net's
    // class 0xF. Matched by page_id, not seq (see the comment on on_error in pager.cpp).
    void on_error(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload);

    // Called only by the installed SIGSEGV handler (segv_trampoline in pager.cpp). Public
    // because a signal handler is a free function with no access to private members, not
    // because application code should ever call this directly.
    void handle_segv(int sig, siginfo_t* info, void* ucontext);

private:
    int acquire_fault_slot();  // async-signal-safe: CAS over faults_[*].in_use only
    void pager_thread_loop();
    void handle_fault(int slot);
    void resolve_slot(int slot, int status);
    void unlink_waiter(PageEntry& entry, uint16_t slot);
    void sweep_timeouts();
    void send_page_request(node_id_t home, page_id_t page, access_t access);
    void complete_grant(page_id_t page, version_t version, page_state_t granted, const uint8_t* data);
    void on_page_data(const Header& h, const uint8_t* payload);
    void on_upgrade_ack(const Header& h);
    void on_invalidate(const std::shared_ptr<Connection>& conn, const Header& h);
    void on_fetch(const std::shared_ptr<Connection>& conn, const Header& h, bool invalidate);
    std::shared_ptr<Connection> data_connection(node_id_t home);

    Memory& mem_;
    Network& data_net_;
    node_id_t my_node_id_ = NODE_NONE;
    uint32_t request_timeout_ms_ = 3000;
    HomeLookup home_lookup_;

    FaultSlot faults_[kQueueSize];
    std::atomic<uint8_t> dispatched_[kQueueSize]{};  // picked up by the pager thread already?
    uint64_t fault_started_ms_[kQueueSize] = {};      // pager-thread-only, no atomics needed
    sem_t pager_sem_{};

    std::mutex conn_lock_;
    std::vector<std::shared_ptr<Connection>> data_conn_;  // per-peer data-channel link, not replicated

    struct sigaction prev_sigaction_{};
    std::thread thread_;
    std::atomic<bool> running_{false};
};
