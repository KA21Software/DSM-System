#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "memory.h"
#include "pager.h"

static int g_failed = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); g_failed++; } } while (0)

// node_id 1 "owns" every page this test touches (it is the home for any page_id >= P, with
// P = pages_per_node from the default Config). Keeping all test pages away from node 0's own
// range means every fault genuinely has to cross send_page_request()/Network, with no risk of
// accidentally depending on a local fast path pager.cpp does not implement (see pager.h).
struct FakeHome {
    Network net;
    std::atomic<int> requests_seen{0};
    std::mutex mu;
    bool reply = true;      // false: silently drop the PAGE_REQUEST, to exercise the timeout
    bool grant_write = false;
    uint32_t next_version = 1;

    int start() {
        net.set_handler(msg_class(PAGE_REQUEST), [this](const std::shared_ptr<Connection>& conn, const Header& h,
                                                          const uint8_t*) {
            requests_seen++;
            std::lock_guard<std::mutex> lk(mu);
            if (!reply) return;

            Header resp;
            resp.type = PAGE_DATA;
            resp.page_id = h.page_id;
            resp.page_version = next_version++;
            resp.flags = grant_write ? F_GRANT_M : 0;
            std::vector<uint8_t> data(4096, grant_write ? 0x4D : 0x53);  // 'M' or 'S', just a marker
            net.send(conn, resp, data.data(), data.size());
        });
        int rc = net.listen(0);
        if (rc != DSM_OK) return rc;
        return net.start();
    }
};

int main() {
    Config cfg;  // defaults: 8 nodes, 1 MiB each, 4096-byte pages -> 256 pages/node
    Memory mem;
    CHECK(mem.init(cfg) == DSM_OK);
    const page_id_t remote_page = mem.first_page_of(1);        // home is node 1, not us
    const page_id_t remote_page2 = mem.first_page_of(1) + 1;   // a second, independent page

    FakeHome home;
    CHECK(home.start() == DSM_OK);

    Network my_net;
    CHECK(my_net.listen(0) == DSM_OK);
    CHECK(my_net.start() == DSM_OK);

    Pager pager(mem, my_net);
    pager.set_my_node_id(0);
    pager.set_request_timeout_ms(1000);
    pager.set_home_lookup([&](node_id_t id, std::string& ip, uint16_t& port) {
        if (id != 1) return false;
        ip = "127.0.0.1";
        port = home.net.listening_port();
        return true;
    });
    my_net.set_handler(msg_class(PAGE_DATA),
                        [&](const std::shared_ptr<Connection>& c, const Header& h, const uint8_t* p) {
                            pager.on_coherence_message(c, h, p);
                        });
    my_net.set_handler(msg_class(KADP_ERROR),
                        [&](const std::shared_ptr<Connection>& c, const Header& h, const uint8_t* p) {
                            pager.on_error(c, h, p);
                        });
    CHECK(pager.start() == DSM_OK);
    CHECK(pager.start() == DSM_EINVAL);  // only one Pager may install the SIGSEGV handler

    // A plain read: the fault blocks this very thread inside the handler until the fake
    // home's PAGE_DATA arrives and installs the page, then the instruction just retries.
    volatile char v = *reinterpret_cast<volatile char*>(mem.user_addr(remote_page));
    CHECK(v == 0x53);
    CHECK(mem.entry(remote_page).state == S);
    CHECK(home.requests_seen.load() == 1);

    // Re-reading the same byte must not fault again (state is already S) or send a second
    // PAGE_REQUEST.
    v = *reinterpret_cast<volatile char*>(mem.user_addr(remote_page));
    CHECK(home.requests_seen.load() == 1);

    // A write fault on an S page asks for an upgrade; the fake home grants M this time.
    {
        std::lock_guard<std::mutex> lk(home.mu);
        home.grant_write = true;
    }
    *reinterpret_cast<volatile char*>(mem.user_addr(remote_page)) = 'x';
    CHECK(mem.entry(remote_page).state == M);
    CHECK(home.requests_seen.load() == 2);

    // Two local threads faulting on the SAME unresolved page at once must result in exactly
    // one PAGE_REQUEST (design section 4.4: no duplicate requests for the same page).
    home.requests_seen = 0;
    std::atomic<int> done{0};
    auto touch = [&] {
        volatile char c = *reinterpret_cast<volatile char*>(mem.user_addr(remote_page2));
        (void)c;
        done++;
    };
    std::thread t1(touch), t2(touch);
    t1.join();
    t2.join();
    CHECK(done.load() == 2);
    CHECK(home.requests_seen.load() == 1);

    // FETCH: the fake home asks back for a page we hold in M. We must reply FETCH_REPLY with
    // the current bytes and drop to S.
    {
        std::atomic<bool> got_fetch_reply{false};
        Header seen{};
        std::vector<uint8_t> seen_payload;
        std::mutex reply_mu;
        home.net.set_handler(msg_class(FETCH_REPLY),
                              [&](const std::shared_ptr<Connection>&, const Header& h, const uint8_t* p) {
                                  std::lock_guard<std::mutex> lk(reply_mu);
                                  seen = h;
                                  seen_payload.assign(p, p + h.payload_len);
                                  got_fetch_reply = true;
                              });

        // remote_page is in M from the write fault above; now pretend the home needs it back.
        auto home_to_me = home.net.connect_to("127.0.0.1", my_net.listening_port(), 1000);
        CHECK(home_to_me != nullptr);
        Header fetch;
        fetch.type = FETCH;
        fetch.page_id = remote_page;
        CHECK(home.net.send(home_to_me, fetch, nullptr, 0) == DSM_OK);

        bool arrived = false;
        for (int i = 0; i < 100 && !arrived; i++) {
            if (got_fetch_reply.load()) arrived = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(arrived);
        CHECK(mem.entry(remote_page).state == S);
        std::lock_guard<std::mutex> lk(reply_mu);
        CHECK(seen.page_id == remote_page && seen_payload.size() == 4096 && seen_payload[0] == 'x');
    }

    // INVALIDATE on an S copy must drop it to I and answer INVALIDATE_ACK.
    {
        std::atomic<bool> got_ack{false};
        home.net.set_handler(msg_class(INVALIDATE_ACK), [&](const std::shared_ptr<Connection>&, const Header& h,
                                                              const uint8_t*) {
            if (h.page_id == remote_page) got_ack = true;
        });
        auto home_to_me = home.net.connect_to("127.0.0.1", my_net.listening_port(), 1000);
        CHECK(home_to_me != nullptr);
        Header inv;
        inv.type = INVALIDATE;
        inv.page_id = remote_page;
        CHECK(home.net.send(home_to_me, inv, nullptr, 0) == DSM_OK);

        bool arrived = false;
        for (int i = 0; i < 100 && !arrived; i++) {
            if (got_ack.load()) arrived = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(arrived);
        CHECK(mem.entry(remote_page).state == I);
    }

    // `pager` and `mem` are done for: stop/unmap them before forking below. Otherwise the
    // child would inherit g_pager still pointing at this process's `pager` (so the wrong
    // Memory/Network would run the handler), and mem's mapping at base_address would still
    // occupy that address in the child, making child_mem.init()'s MAP_FIXED_NOREPLACE fail.
    pager.stop();
    mem.shutdown();

    // A fault that never gets an answer (no route to the home at all, here) must resolve
    // with DSM_ETIMEOUT and abort() — the documented behaviour for an unresolved fault
    // (pager.cpp's handle_segv). Run in a forked child, since aborting would otherwise end
    // this whole test; it gets its own fresh Pager/Memory pair, started only after the fork.
    pid_t pid = fork();
    if (pid == 0) {
        Config child_cfg;
        Memory child_mem;
        if (child_mem.init(child_cfg) != DSM_OK) _exit(2);
        Network child_net;
        if (child_net.listen(0) != DSM_OK || child_net.start() != DSM_OK) _exit(2);
        Pager child_pager(child_mem, child_net);
        child_pager.set_request_timeout_ms(200);  // short: this test should not hang
        child_pager.set_home_lookup([](node_id_t, std::string&, uint16_t&) { return false; });  // no route at all
        if (child_pager.start() != DSM_OK) _exit(2);

        page_id_t p = child_mem.first_page_of(1);
        volatile char c = *reinterpret_cast<volatile char*>(child_mem.user_addr(p));  // must abort()
        (void)c;
        _exit(0);  // unreachable if the fault is handled as designed
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

    // An out-of-range access (outside the reserved space entirely) must still behave like a
    // normal segfault: the default handler kills the process with SIGSEGV, not SIGABRT.
    pid = fork();
    if (pid == 0) {
        Config child_cfg;
        Memory child_mem;
        if (child_mem.init(child_cfg) != DSM_OK) _exit(2);
        Network child_net;
        if (child_net.listen(0) != DSM_OK || child_net.start() != DSM_OK) _exit(2);
        Pager child_pager(child_mem, child_net);
        if (child_pager.start() != DSM_OK) _exit(2);

        volatile char* bogus = reinterpret_cast<volatile char*>(0x123000);  // never inside our range
        volatile char c = *bogus;
        (void)c;
        _exit(0);
    }
    status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);

    my_net.stop();
    home.net.stop();
    mem.shutdown();

    if (g_failed) { fprintf(stderr, "%d check(s) failed\n", g_failed); return 1; }
    puts("test_pager OK");
    return 0;
}
