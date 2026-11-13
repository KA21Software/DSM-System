#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "network.h"

static int g_failed = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); g_failed++; } } while (0)

int main() {
    // send_all/recv_all over a real socket pair, bypassing Network: a payload bigger than
    // one send()/recv() call must still arrive whole, and a peer that closes early must fail
    // cleanly instead of hanging or returning a short read as success.
    {
        int fds[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        std::vector<uint8_t> big(200000, 0x5A);
        std::thread writer([&] { CHECK(send_all(fds[0], big.data(), big.size())); });
        std::vector<uint8_t> got(big.size());
        CHECK(recv_all(fds[1], got.data(), got.size()));
        writer.join();
        CHECK(got == big);
        close(fds[0]);
        close(fds[1]);
    }
    {
        int fds[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        CHECK(send_all(fds[0], "hi", 2));
        close(fds[0]);  // closes before the 3rd byte test_network is about to ask for
        uint8_t buf[3];
        CHECK(!recv_all(fds[1], buf, 3));
        close(fds[1]);
    }

    // End to end: two Network instances on 127.0.0.1 exchange a coherence-class request and
    // reply, dispatched purely by msg_class() with no knowledge of each other's node_id.
    Network server, client;
    CHECK(server.listen(0) == DSM_OK);  // port 0: let the OS pick one, avoids clashing ports
    uint16_t port = server.listening_port();
    CHECK(port != 0);
    CHECK(server.start() == DSM_OK);
    CHECK(server.start() == DSM_EINVAL);  // already running
    CHECK(client.start() == DSM_OK);

    std::mutex mu;
    std::condition_variable cv;
    bool got_request = false, got_reply = false;
    Header seen_req, seen_reply;
    std::vector<uint8_t> seen_payload;

    server.set_handler(msg_class(PAGE_REQUEST),
                        [&](const std::shared_ptr<Connection>& from, const Header& h, const uint8_t*) {
                            {
                                std::lock_guard<std::mutex> lk(mu);
                                seen_req = h;
                                got_request = true;
                            }
                            cv.notify_all();
                            uint8_t data[4] = {1, 2, 3, 4};
                            Header reply;
                            reply.type = PAGE_DATA;
                            reply.seq = h.seq;
                            reply.page_id = h.page_id;
                            server.send(from, reply, data, sizeof data);
                        });
    client.set_handler(msg_class(PAGE_DATA),
                        [&](const std::shared_ptr<Connection>&, const Header& h, const uint8_t* payload) {
                            {
                                std::lock_guard<std::mutex> lk(mu);
                                seen_reply = h;
                                seen_payload.assign(payload, payload + h.payload_len);
                                got_reply = true;
                            }
                            cv.notify_all();
                        });

    auto conn = client.connect_to("127.0.0.1", port, 2000);
    CHECK(conn != nullptr);
    Header req;
    req.type = PAGE_REQUEST;
    req.seq = 42;
    req.page_id = 600;
    CHECK(client.send(conn, req, nullptr, 0) == DSM_OK);

    {
        std::unique_lock<std::mutex> lk(mu);
        CHECK(cv.wait_for(lk, std::chrono::seconds(2), [&] { return got_request && got_reply; }));
    }
    CHECK(seen_req.seq == 42 && seen_req.page_id == 600);
    CHECK(seen_reply.seq == 42 && seen_reply.page_id == 600 && seen_reply.payload_len == 4);
    CHECK(seen_payload.size() == 4 && seen_payload[2] == 3);

    // A message with no handler registered (here, any 0x2_ code) must be dropped quietly,
    // not crash the listener thread or wedge the connection for later messages.
    Header unhandled;
    unhandled.type = ALLOC_REQUEST;
    unhandled.seq = 7;
    CHECK(client.send(conn, unhandled, nullptr, 0) == DSM_OK);
    {
        got_request = false;
        Header again = req;
        again.seq = 43;
        CHECK(client.send(conn, again, nullptr, 0) == DSM_OK);
        std::unique_lock<std::mutex> lk(mu);
        CHECK(cv.wait_for(lk, std::chrono::seconds(2), [&] { return got_request; }));
    }
    CHECK(seen_req.seq == 43);

    // Connecting to a closed port must fail within timeout_ms, not hang.
    Network nobody;
    CHECK(nobody.listen(0) == DSM_OK);
    uint16_t dead_port = nobody.listening_port();
    nobody.stop();  // listens, then immediately closes: the port is refused, not just slow
    auto start = std::chrono::steady_clock::now();
    auto bad = client.connect_to("127.0.0.1", dead_port, 1000);
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(bad == nullptr);
    CHECK(elapsed < std::chrono::seconds(1));

    if (g_failed) { fprintf(stderr, "%d check(s) failed\n", g_failed); return 1; }
    puts("test_network OK");
    return 0;
}
