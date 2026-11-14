#include "network.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "log.h"

bool send_all(int fd, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recv_all(int fd, void* data, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(data);
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;  // peer closed before the message was complete
        got += static_cast<size_t>(n);
    }
    return true;
}

Connection::~Connection() { close_now(); }

void Connection::close_now() {
    // Shares send_lock with Network::send(): otherwise this could close the fd while another
    // thread's send() is blocked inside write()/send() on it, and the kernel is then free to
    // hand that same fd number to a brand new, unrelated socket before the write returns.
    std::lock_guard<std::mutex> lk(send_lock);
    if (!closed.exchange(true)) close(fd);
}

namespace {
// Small messages sent one at a time; without this, Linux may hold a partial header in the
// kernel buffer for tens of milliseconds waiting to coalesce it with more data (section 6.1).
void enable_nodelay(int fd) {
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
}
}  // namespace

int Network::listen(uint16_t port) {
    if (listen_fd_ >= 0) return DSM_EINVAL;  // already bound; stop() first to rebind

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("socket() failed: %s", strerror(errno));
        return DSM_ENET;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);  // quick restart while developing

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    socklen_t addr_len = sizeof addr;
    // ::listen, not listen: this is Network::listen, and an unqualified call would recurse.
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), addr_len) != 0 || ::listen(fd, 16) != 0 ||
        getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &addr_len) != 0) {
        LOG_ERROR("listen on port %u failed: %s", port, strerror(errno));
        close(fd);
        return DSM_ENET;
    }

    listen_fd_ = fd;
    bound_port_ = ntohs(addr.sin_port);
    LOG_INFO("listening on port %u (fd %d)", bound_port_, fd);
    return DSM_OK;
}

std::shared_ptr<Connection> Network::connect_to(const std::string& ip, uint16_t port, uint32_t timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("socket() failed: %s", strerror(errno));
        return nullptr;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        // Not a dotted IPv4 literal: cluster.hosts may list a hostname instead (design 8.2).
        addrinfo hints{};
        hints.ai_family = AF_INET;  // IPv4 only, per design section 12.2
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(ip.c_str(), nullptr, &hints, &res) != 0 || !res) {
            LOG_ERROR("cannot resolve host '%s'", ip.c_str());
            close(fd);
            return nullptr;
        }
        addr.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    // Non-blocking connect bounded by timeout_ms, instead of the kernel's multi-minute
    // default: a node that is not there yet must not stall the caller (e.g. the JOIN probe
    // trying every host in order).
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    if (rc != 0 && errno != EINPROGRESS) {
        LOG_WARN("connect to %s:%u failed: %s", ip.c_str(), port, strerror(errno));
        close(fd);
        return nullptr;
    }
    if (rc != 0) {
        pollfd pfd{fd, POLLOUT, 0};
        int perr = 0;
        socklen_t elen = sizeof perr;
        if (poll(&pfd, 1, static_cast<int>(timeout_ms)) <= 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &perr, &elen) != 0 || perr != 0) {
            LOG_WARN("connect to %s:%u timed out or refused", ip.c_str(), port);
            close(fd);
            return nullptr;
        }
    }
    fcntl(fd, F_SETFL, flags);  // back to blocking: service()/send_all() assume that

    enable_nodelay(fd);
    auto conn = std::make_shared<Connection>(fd);
    conn->peer_ip = ip;
    conn->peer_port = port;
    {
        std::lock_guard<std::mutex> lk(conns_lock_);
        conns_.push_back(conn);
    }
    LOG_INFO("connected to %s:%u (fd %d)", ip.c_str(), port, fd);
    return conn;
}

void Network::set_handler(uint8_t cls, Handler handler) {
    std::lock_guard<std::mutex> lk(handlers_lock_);
    handlers_[cls & 0xF] = std::move(handler);
}

int Network::send(const std::shared_ptr<Connection>& conn, Header h, const void* payload, size_t len) {
    if (!conn) return DSM_ENET;

    std::vector<uint8_t> frame;
    int rc = frame_build(h, payload, len, frame);
    if (rc != DSM_OK) return rc;

    // closed is only safe to trust once send_lock is held: close_now() takes the same lock,
    // so no close() can land between this check and send_all() below.
    std::lock_guard<std::mutex> lk(conn->send_lock);
    if (conn->closed.load()) return DSM_ENET;
    if (!send_all(conn->fd, frame.data(), frame.size())) {
        // Not conn->close_now(): it takes send_lock too, and it is already held here.
        if (!conn->closed.exchange(true)) close(conn->fd);
        return DSM_ENET;
    }
    bytes_sent_ += frame.size();
    return DSM_OK;
}

void Network::accept_one() {
    sockaddr_in peer{};
    socklen_t plen = sizeof peer;
    int fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
    if (fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) LOG_WARN("accept() failed: %s", strerror(errno));
        return;
    }
    enable_nodelay(fd);

    auto conn = std::make_shared<Connection>(fd);
    char ipbuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer.sin_addr, ipbuf, sizeof ipbuf);
    conn->peer_ip = ipbuf;
    conn->peer_port = ntohs(peer.sin_port);

    std::lock_guard<std::mutex> lk(conns_lock_);
    conns_.push_back(conn);
    LOG_INFO("accepted connection from %s:%u (fd %d)", conn->peer_ip.c_str(), conn->peer_port, fd);
}

bool Network::service(const std::shared_ptr<Connection>& conn) {
    uint8_t raw[KADP_HDR_LEN];
    if (!recv_all(conn->fd, raw, KADP_HDR_LEN)) return false;  // peer closed or a read error

    Header h;
    if (header_decode(raw, h) != DSM_OK) {
        // Wrong magic/version or an impossible payload_len: framing can no longer be
        // trusted, so the connection is dropped rather than skipping just this message.
        LOG_WARN("bad header from %s:%u, closing connection", conn->peer_ip.c_str(), conn->peer_port);
        return false;
    }

    std::vector<uint8_t> payload;
    if (h.payload_len > 0) {
        payload.resize(h.payload_len);
        if (!recv_all(conn->fd, payload.data(), h.payload_len)) return false;
    }
    const uint8_t* payload_ptr = h.payload_len ? payload.data() : nullptr;
    bytes_received_ += KADP_HDR_LEN + h.payload_len;

    if (payload_verify(h, payload_ptr) != DSM_OK) {
        // The frame itself was well-formed, only the payload was damaged: per section 6.2,
        // answer ERROR and keep the connection (the sender is expected to retry).
        LOG_WARN("checksum mismatch from %s:%u, seq %u", conn->peer_ip.c_str(), conn->peer_port, h.seq);
        Header err;
        err.type = KADP_ERROR;
        err.seq = h.seq;
        err.status = DSM_EPROTO;
        send(conn, err, nullptr, 0);
        return true;
    }

    Handler handler;
    {
        std::lock_guard<std::mutex> lk(handlers_lock_);
        handler = handlers_[msg_class(h.type)];
    }
    if (handler) handler(conn, h, payload_ptr);
    else LOG_DEBUG("no handler registered for message type 0x%02x, dropped", h.type);
    return true;
}

void Network::run() {
    while (running_.load()) {
        std::vector<std::shared_ptr<Connection>> snapshot;
        {
            std::lock_guard<std::mutex> lk(conns_lock_);
            conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                         [](const std::shared_ptr<Connection>& c) { return c->closed.load(); }),
                         conns_.end());
            snapshot = conns_;
        }

        std::vector<pollfd> fds;
        fds.reserve(snapshot.size() + 1);
        if (listen_fd_ >= 0) fds.push_back({listen_fd_, POLLIN, 0});
        for (auto& c : snapshot) fds.push_back({c->fd, POLLIN, 0});

        // Short timeout: a connection added by connect_to() on another thread only joins
        // this poll() on its next iteration, so the wait cannot be unbounded.
        int rc = poll(fds.data(), fds.size(), 50);
        if (rc < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("poll() failed: %s", strerror(errno));
            break;
        }
        if (rc == 0) continue;

        size_t idx = 0;
        if (listen_fd_ >= 0) {
            if (fds[idx].revents & POLLIN) accept_one();
            idx++;
        }
        for (auto& c : snapshot) {
            if (fds[idx].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (!service(c)) c->close_now();
            }
            idx++;
        }
    }
}

int Network::start() {
    if (running_.exchange(true)) return DSM_EINVAL;
    thread_ = std::thread(&Network::run, this);
    return DSM_OK;
}

void Network::stop() {
    // running_ only gates the thread join: listen() or connect_to() without start() (as in a
    // test, or a node that failed to join the cluster) must still release the fd on stop().
    bool was_running = running_.exchange(false);
    if (was_running && thread_.joinable()) thread_.join();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    std::lock_guard<std::mutex> lk(conns_lock_);
    for (auto& c : conns_) c->close_now();
    conns_.clear();
}
