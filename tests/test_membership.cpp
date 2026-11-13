#include <chrono>
#include <cstdio>
#include <thread>

#include "log.h"
#include "membership.h"

static int g_failed = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); g_failed++; } } while (0)

// Small, fast values so the test does not have to sit through real-world intervals: two
// nodes on 127.0.0.1, at most 2 per host so the slot search space stays tiny.
static Config test_config() {
    Config cfg;
    cfg.hosts = {"127.0.0.1"};
    cfg.max_nodes = 4;
    cfg.max_nodes_per_host = 2;
    cfg.port_base = 29100;  // outside the lab's assigned 28100-28199, avoids clashing with it
    cfg.port_stride = 10;
    cfg.connect_timeout_ms = 300;
    cfg.request_timeout_ms = 600;
    cfg.hb_interval_ms = 80;
    cfg.hb_suspect_after = 2;
    cfg.hb_dead_after = 3;
    return cfg;
}

int main() {
    log_init("./logs", "DEBUG", "test_membership");  // ./logs is already gitignored
    Config cfg = test_config();

    // Node A starts alone: nobody answers its probe, so it founds the cluster as id 0.
    Membership a(cfg);
    CHECK(a.join() == DSM_OK);
    CHECK(a.my_id() == 0 && a.slot() == 0 && a.is_coordinator());
    CHECK(a.snapshot().size() == 1);

    // Node B starts next: slot 0's ports are taken, so it must fall into slot 1, then find A
    // on slot 0 and join as id 1. Both run join() on their own thread because join() blocks
    // on the probe/timeout sequence above.
    Membership b(cfg);
    int b_rc = DSM_ETIMEOUT;
    std::thread b_join([&] { b_rc = b.join(); });
    b_join.join();

    CHECK(b_rc == DSM_OK);
    CHECK(b.my_id() == 1 && b.slot() == 1 && !b.is_coordinator());
    CHECK(b.snapshot().size() == 2);

    // A's table must have picked up B via NODE_ANNOUNCE (nothing re-fetches it afterwards).
    bool a_sees_b = false;
    for (int i = 0; i < 50 && !a_sees_b; i++) {
        if (a.snapshot().size() == 2) a_sees_b = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(a_sees_b);
    NodeInfo b_from_a{};
    CHECK(a.node_info(1, b_from_a) && b_from_a.slot == 1);

    // Heartbeat visible in practice: last_hb_ms on each side keeps advancing on its own.
    NodeInfo before{};
    CHECK(a.node_info(1, before));
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg.hb_interval_ms * 3));
    NodeInfo after{};
    CHECK(a.node_info(1, after));
    CHECK(after.last_hb_ms > before.last_hb_ms);
    CHECK(after.state == ALIVE && after.missed_hb == 0);

    // B goes silent (simulating a crash, not a clean LEAVE): A must notice via missed
    // heartbeats and mark it SUSPECT, exactly as section 7.3 describes.
    b.stop();
    bool suspected = false;
    for (int i = 0; i < 100 && !suspected; i++) {
        NodeInfo n{};
        if (a.node_info(1, n) && n.state == SUSPECT) suspected = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(suspected);

    // A config_hash mismatch (different page_size here) must be rejected immediately, not
    // time out: the coordinator does exist and answers, it just says no.
    Config bad_cfg = test_config();
    bad_cfg.page_size = 8192;
    Membership c(bad_cfg);
    auto start = std::chrono::steady_clock::now();
    int c_rc = DSM_OK;
    std::thread c_join([&] { c_rc = c.join(); });
    c_join.join();
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(c_rc == DSM_ECONFIG);
    CHECK(elapsed < std::chrono::milliseconds(cfg.request_timeout_ms * 2));

    // A full cluster (max_nodes already used) must reject a new JOIN with DSM_EFULL. Needs
    // its own cluster (same config_hash on both sides, which max_nodes feeds into) and its
    // own port range so it does not collide with A/B, still running above.
    Config full_cfg = test_config();
    full_cfg.max_nodes = 1;
    full_cfg.port_base = 29200;
    Membership e(full_cfg);
    CHECK(e.join() == DSM_OK && e.snapshot().size() == 1);  // founds it, uses the only id there is

    Membership d(full_cfg);
    int d_rc = DSM_OK;
    std::thread d_join([&] { d_rc = d.join(); });
    d_join.join();
    CHECK(d_rc == DSM_EFULL);

    a.stop();
    c.stop();
    d.stop();
    e.stop();

    if (g_failed) { fprintf(stderr, "%d check(s) failed\n", g_failed); return 1; }
    puts("test_membership OK");
    return 0;
}
