// Minimal HTTP/1.0 server for the HTML view of the console commands (design section 10.3).
// One thread, one request at a time, no libraries: it only turns a path into a console
// command and wraps the text in a page that reloads itself every 2 seconds.
#pragma once
#include <atomic>
#include <thread>

#include "query.h"

class Web {
public:
    explicit Web(Query& query) : query_(query) {}
    ~Web() { stop(); }
    // False if the port cannot be bound (the node keeps running without the page).
    bool start(uint16_t port);
    void stop();

private:
    void loop();
    void serve(int fd);

    Query& query_;
    int listen_fd_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};
};
