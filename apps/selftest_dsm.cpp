// Cluster self-test: the concurrency scenarios of design table 4.3 plus allocation, names,
// locks and statistics, run by every node of a cluster at once.
//
// Usage: selftest_dsm <dsm.conf> <nodes> [notalloc]
// Start it once per node (tests/test_cluster.sh does). Every node prints PASS or FAIL lines
// and exits 0 only if all of its checks passed. With "notalloc" the program instead touches a
// page nobody allocated and is expected to die with DSM_ENOTALLOC.
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "dsm.h"

namespace {

int me, N, failures = 0;
constexpr size_t kPage = 4096;

#define CHECK(cond, ...)                                      \
    do {                                                      \
        if (!(cond)) {                                        \
            failures++;                                       \
            printf("[node %d] FAIL line %d: ", me, __LINE__); \
            printf(__VA_ARGS__);                              \
            printf("\n");                                     \
        }                                                     \
    } while (0)

void sync(uint32_t id) {
    int rc = dsm_barrier(id, static_cast<uint16_t>(N));
    if (rc != 0) {
        printf("[node %d] FAIL barrier %u: %s\n", me, id, dsm_strerror(rc));
        fflush(stdout);
        _exit(3);
    }
}

void* named(const char* name, size_t size) {
    void* p = dsm_alloc_named(name, size);
    if (!p) {
        printf("[node %d] FAIL cannot allocate '%s': %s\n", me, name, dsm_strerror(dsm_last_error()));
        fflush(stdout);
        _exit(3);
    }
    return p;
}

// Scenario 1: every node updates its own word of the same page, many times, at the same time.
void t_same_page() {
    volatile uint32_t* w = static_cast<volatile uint32_t*>(named("t1", kPage));
    for (int i = 0; i < 500; i++) w[me] = w[me] + 1;
    sync(10);
    for (int k = 0; k < N; k++) CHECK(w[k] == 500, "word %d is %u, expected 500", k, w[k]);
    printf("[node %d] t1 concurrent writes on one page: done\n", me);
}

// Scenario 2: everybody holds the page in S, then everybody asks to write at once.
void t_upgrade() {
    volatile uint32_t* w = static_cast<volatile uint32_t*>(named("t2", kPage));
    uint32_t seen = w[0] + w[1];  // S copy everywhere
    (void)seen;
    sync(20);
    w[me] = me + 1;  // SM_P on every node; the home orders them, losers get PAGE_DATA(M)
    sync(21);
    for (int k = 0; k < N; k++) CHECK(w[k] == static_cast<uint32_t>(k + 1), "word %d is %u", k, w[k]);
    printf("[node %d] t2 crossed upgrades: done\n", me);
}

// Scenario 3: several threads of the same node fault on the same fresh pages.
void t_threads() {
    constexpr int kPages = 64, kThreads = 8;
    volatile uint32_t* w = static_cast<volatile uint32_t*>(named("t3", kPages * kPage));
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; t++)
        ts.emplace_back([=] {
            for (int p = 0; p < kPages; p++) w[p * 1024 + me * kThreads + t] = static_cast<uint32_t>(me * 1000 + t * 10 + p);
        });
    for (auto& t : ts) t.join();
    sync(30);
    bool ok = true;
    for (int n = 0; n < N && ok; n++)
        for (int t = 0; t < kThreads && ok; t++)
            for (int p = 0; p < kPages && ok; p++)
                if (w[p * 1024 + n * kThreads + t] != static_cast<uint32_t>(n * 1000 + t * 10 + p)) {
                    CHECK(false, "t3 page %d node %d thread %d wrong", p, n, t);
                    ok = false;
                }
    printf("[node %d] t3 duplicate faults from threads: done\n", me);
}

// Locks: a counter incremented by every node under dsm_lock.
void t_lock() {
    volatile uint64_t* c = static_cast<volatile uint64_t*>(named("t4", 8));
    constexpr int kIters = 200;
    for (int i = 0; i < kIters; i++) {
        CHECK(dsm_lock(7) == 0, "lock");
        *c = *c + 1;
        CHECK(dsm_unlock(7) == 0, "unlock");
    }
    sync(40);
    CHECK(*c == static_cast<uint64_t>(N) * kIters, "counter is %llu, expected %d", static_cast<unsigned long long>(*c), N * kIters);
    CHECK(dsm_unlock(7) == DSM_EINVAL, "unlock of a lock not held must fail");
    printf("[node %d] t4 lock counter = %llu: done\n", me, static_cast<unsigned long long>(*c));
}

// Scenario 5: each node writes a page homed at its neighbour while the neighbour does the same.
void t_cross_homes() {
    volatile uint64_t* ptrs = static_cast<volatile uint64_t*>(named("t5ptr", 64 * sizeof(uint64_t)));
    uint8_t* mine = static_cast<uint8_t*>(dsm_alloc(kPage));  // lands in this node's own range
    CHECK(mine != nullptr, "alloc");
    ptrs[me] = reinterpret_cast<uint64_t>(mine);
    sync(50);
    volatile uint8_t* next = reinterpret_cast<uint8_t*>(ptrs[(me + 1) % N]);
    volatile uint8_t* prev = reinterpret_cast<uint8_t*>(ptrs[(me + N - 1) % N]);
    for (int i = 0; i < 100; i++) {
        next[me] = static_cast<uint8_t>(i);
        uint8_t sink = prev[(me + N - 1) % N];
        (void)sink;
    }
    sync(51);
    CHECK(next[me] == 99, "cross write lost");
    printf("[node %d] t5 cross-home access: done\n", me);
}

// Names: the same name from every node is one block.
void t_names() {
    volatile uint64_t* ptrs = static_cast<volatile uint64_t*>(named("t6ptr", 64 * sizeof(uint64_t)));
    void* p = named("shared_name", 10000);
    ptrs[me] = reinterpret_cast<uint64_t>(p);
    sync(60);
    for (int k = 0; k < N; k++) CHECK(ptrs[k] == reinterpret_cast<uint64_t>(p), "node %d got another block for the same name", k);
    size_t size = 0;
    CHECK(dsm_lookup("shared_name", &size) == p && size == 10000, "lookup");
    CHECK(dsm_alloc_named("shared_name", 20000) == nullptr && dsm_last_error() == DSM_EEXIST, "different size must be DSM_EEXIST");
    CHECK(dsm_lookup("no_such_name", nullptr) == nullptr && dsm_last_error() == DSM_ENOTFOUND, "missing name");
    printf("[node %d] t6 names: done\n", me);
}

// free: contents seen by other nodes must not survive into the next allocation.
void t_free() {
    volatile uint64_t* ptrs = static_cast<volatile uint64_t*>(named("t7ptr", 64 * sizeof(uint64_t)));
    if (me == 0) {
        uint8_t* b = static_cast<uint8_t*>(dsm_alloc(3 * kPage));
        CHECK(b != nullptr, "alloc");
        memset(b, 0xAB, 3 * kPage);
        ptrs[0] = reinterpret_cast<uint64_t>(b);
    }
    sync(70);
    volatile uint8_t* b = reinterpret_cast<uint8_t*>(ptrs[0]);
    for (size_t i = 0; i < 3 * kPage; i += 512) CHECK(b[i] == 0xAB, "pattern at %zu", i);  // S copies on every node
    sync(71);
    if (me == 0) {
        CHECK(dsm_free(const_cast<uint8_t*>(b)) == 0, "free");
        CHECK(dsm_free(const_cast<uint8_t*>(b)) != 0, "second free must fail");
        uint8_t* b2 = static_cast<uint8_t*>(dsm_alloc(3 * kPage));
        CHECK(b2 != nullptr, "realloc");
        ptrs[1] = reinterpret_cast<uint64_t>(b2);
        printf("[node 0] t7 block reused at the same address: %s\n", b2 == b ? "yes" : "no");
    }
    sync(72);
    volatile uint8_t* b2 = reinterpret_cast<uint8_t*>(ptrs[1]);
    for (size_t i = 0; i < 3 * kPage; i += 512) CHECK(b2[i] == 0, "stale byte 0x%02x at %zu after free", b2[i], i);
    printf("[node %d] t7 free and reallocate: done\n", me);
}

// An allocation that does not fit in one range, and one that does not fit anywhere.
void t_big() {
    volatile uint64_t* ptrs = static_cast<volatile uint64_t*>(named("t8ptr", 64 * sizeof(uint64_t)));
    const size_t range = dsm_home_size();
    if (me == 0) {
        size_t big = range + 10 * kPage;  // needs two ranges
        uint8_t* p = static_cast<uint8_t*>(dsm_alloc(big));
        CHECK(p != nullptr, "spanning alloc failed: %s", dsm_strerror(dsm_last_error()));
        if (p) {
            p[0] = 1;
            p[big - 1] = 2;
            p[range] = 3;  // first byte of the second range
            ptrs[0] = reinterpret_cast<uint64_t>(p);
            ptrs[1] = big;
        }
        CHECK(dsm_alloc(dsm_space_size() + kPage) == nullptr && dsm_last_error() == DSM_ENOMEM, "must be DSM_ENOMEM");
        CHECK(dsm_alloc(0) == nullptr && dsm_last_error() == DSM_EINVAL, "size 0 must be DSM_EINVAL");
    }
    sync(80);
    volatile uint8_t* p = reinterpret_cast<uint8_t*>(ptrs[0]);
    size_t big = ptrs[1];
    if (p) CHECK(p[0] == 1 && p[big - 1] == 2 && p[range] == 3, "spanning block contents");
    sync(81);
    if (me == 0 && p) CHECK(dsm_free(const_cast<uint8_t*>(p)) == 0, "free of the spanning block");
    printf("[node %d] t8 spanning allocation: done\n", me);
}

void t_stats() {
    dsm_stats_t s, g;
    CHECK(dsm_get_stats(&s) == 0, "stats");
    CHECK(s.faults_read + s.faults_write > 0 && s.pages_received > 0, "no faults counted");
    CHECK(dsm_get_global_stats(&g) == 0, "global stats");
    CHECK(g.n_nodes_alive == N && g.faults_write >= s.faults_write, "global stats do not add up");
    if (N > 1) {
        dsm_stats_t other;
        CHECK(dsm_get_node_stats((me + 1) % N, &other) == 0 && other.node_id == (me + 1) % N, "remote stats");
    }
    printf("[node %d] t9 stats: %llu read faults, %llu write faults, avg %u us\n", me,
           static_cast<unsigned long long>(s.faults_read), static_cast<unsigned long long>(s.faults_write), s.avg_fault_us);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <dsm.conf> <nodes> [notalloc]\n", argv[0]);
        return 1;
    }
    N = atoi(argv[2]);
    int rc = dsm_init(argv[1]);
    if (rc != 0) {
        fprintf(stderr, "dsm_init: %s\n", dsm_strerror(rc));
        return 1;
    }
    me = dsm_node_id();
    while (dsm_num_nodes() < N) usleep(100000);

    if (argc > 3 && !strcmp(argv[3], "notalloc")) {
        // Nothing was allocated in the last page of the space: the home must refuse it.
        volatile char* p = static_cast<volatile char*>(dsm_home_base(0)) + dsm_space_size() - 1;
        printf("[node %d] touching an unallocated page...\n", me);
        fflush(stdout);
        char c = *p;
        printf("[node %d] FAIL: read %d from an unallocated page\n", me, c);
        return 1;
    }

    t_same_page();
    t_upgrade();
    t_threads();
    t_lock();
    t_cross_homes();
    t_names();
    t_free();
    t_big();
    t_stats();
    sync(99);
    printf("[node %d] SELFTEST %s\n", me, failures ? "FAIL" : "PASS");
    fflush(stdout);
    dsm_shutdown();
    return failures ? 1 : 0;
}
