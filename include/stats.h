// Process-wide counters behind dsm_stats_t (design section 9.2). One node per process, so a
// single instance is enough; every field is a relaxed atomic bumped from any thread.
#pragma once
#include <atomic>
#include <cstdint>

struct Counters {
    std::atomic<uint64_t> faults_read{0}, faults_write{0};
    std::atomic<uint64_t> pages_sent{0}, pages_received{0};
    std::atomic<uint64_t> invalidations_sent{0}, invalidations_received{0};
    std::atomic<uint64_t> fault_us_total{0}, fault_count{0}, fault_us_max{0};

    void add(std::atomic<uint64_t>& c, uint64_t n = 1) { c.fetch_add(n, std::memory_order_relaxed); }
    void note_fault_latency(uint64_t us) {
        add(fault_us_total, us);
        add(fault_count);
        uint64_t m = fault_us_max.load(std::memory_order_relaxed);
        while (us > m && !fault_us_max.compare_exchange_weak(m, us, std::memory_order_relaxed)) {}
    }
};

Counters& counters();
