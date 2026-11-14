// TCP transport for KADP (design document, section 6.1). A Network instance owns one
// listening port and every connection that uses it. Callers (membership, pager, directory)
// register a handler per message class and get a Connection to answer on, or to speak first
// with connect_to(). This module knows nothing about node_id: mapping a node to its
// Connection is the caller's job, because that mapping only exists once JOIN has happened.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "protocol.h"

// Loop over short reads/writes, which TCP may deliver in pieces even for small messages.
// Both return false on error or on a peer that closed before the transfer completed.
bool send_all(int fd, const void* data, size_t len);
bool recv_all(int fd, void* data, size_t len);

// One persistent socket to a peer. send_lock serialises a whole message (header + payload)
// so two threads writing to the same peer never interleave their bytes (section 4.1, send_lock).
struct Connection {
    int fd;
    std::mutex send_lock;
    std::atomic<bool> closed{false};
    std::string peer_ip;
    uint16_t peer_port = 0;

    explicit Connection(int socket_fd) : fd(socket_fd) {}
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Idempotent: safe to call from a send() failure, from the listener thread and from the
    // destructor without risking a double close() on the same fd.
    void close_now();
};

class Network {
public:
    // Invoked on the listener thread for every message whose msg_class() matches `cls`.
    // `conn` is the connection the message arrived on; sending on it reuses that same socket.
    using Handler = std::function<void(const std::shared_ptr<Connection>& conn, const Header& h,
                                        const uint8_t* payload)>;

    Network() = default;
    ~Network() { stop(); }
    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    // Binds and listens. port = 0 lets the OS pick one (used by tests); listening_port()
    // then returns the real value. Must be called before start() if this node is to accept.
    int listen(uint16_t port);
    uint16_t listening_port() const { return bound_port_; }

    // Dials out to ip:port (dotted IPv4 or a hostname that resolves to one). Blocks at most
    // timeout_ms. Returns nullptr on failure; the connection is already tracked on success.
    std::shared_ptr<Connection> connect_to(const std::string& ip, uint16_t port, uint32_t timeout_ms);

    // Registers (or replaces) the handler for a message class (type >> 4, so 0x0 to 0xF).
    void set_handler(uint8_t cls, Handler handler);

    // Builds the frame and sends it under conn->send_lock. DSM_ENET closes the connection
    // (the listener thread stops polling it on its next iteration); DSM_EINVAL on a bad
    // payload (see frame_build). Safe to call from inside a Handler.
    int send(const std::shared_ptr<Connection>& conn, Header h, const void* payload, size_t len);

    // Starts the listener thread: accepts new peers and services every open connection
    // through one poll() loop. DSM_EINVAL if already started.
    int start();
    void stop();

    // Frame bytes (header included) that crossed this instance's sockets, for dsm_stats_t.
    uint64_t bytes_sent() const { return bytes_sent_.load(); }
    uint64_t bytes_received() const { return bytes_received_.load(); }

private:
    void run();
    void accept_one();
    bool service(const std::shared_ptr<Connection>& conn);  // false => drop the connection

    int listen_fd_ = -1;
    uint16_t bound_port_ = 0;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> bytes_sent_{0}, bytes_received_{0};

    std::mutex conns_lock_;
    std::vector<std::shared_ptr<Connection>> conns_;

    std::mutex handlers_lock_;
    Handler handlers_[16];
};
