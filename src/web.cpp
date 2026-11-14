#include "web.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include "log.h"

namespace {

std::string html_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '&') o += "&amp;";
        else o += c;
    }
    return o;
}

struct Route {
    const char* path;
    const char* command;
    const char* title;
};
const Route kRoutes[] = {{"/", "nodes", "Nodos"},
                         {"/stats", "stats", "Estadisticas de este nodo"},
                         {"/stats/global", "stats global", "Estadisticas globales"},
                         {"/pages", "pages local", "Paginas locales"},
                         {"/pages/global", "pages global", "Directorios (todos los homes)"},
                         {"/allocs", "allocs", "Asignaciones"},
                         {"/names", "names", "Nombres"}};

}  // namespace

bool Web::start(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(fd, 8) != 0) {
        LOG_WARN("web: cannot listen on port %u: %s", port, strerror(errno));
        close(fd);
        return false;
    }
    listen_fd_ = fd;
    running_ = true;
    thread_ = std::thread(&Web::loop, this);
    LOG_INFO("web view on http://localhost:%u/", port);
    return true;
}

void Web::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (listen_fd_ >= 0) close(listen_fd_);
    listen_fd_ = -1;
}

void Web::loop() {
    while (running_.load()) {
        pollfd p{listen_fd_, POLLIN, 0};
        if (poll(&p, 1, 200) <= 0) continue;
        int fd = accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) continue;
        timeval tv{2, 0};  // a silent client must not hold the only web thread
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        serve(fd);
        close(fd);
    }
}

void Web::serve(int fd) {
    char req[1024];
    ssize_t n = recv(fd, req, sizeof req - 1, 0);
    if (n <= 0) return;
    req[n] = 0;
    std::string line(req, strcspn(req, "\r\n"));
    std::string path = line.size() > 4 && line.compare(0, 4, "GET ") == 0 ? line.substr(4, line.find(' ', 4) - 4) : "";

    std::string status = "200 OK", body;
    const Route* route = nullptr;
    for (const Route& r : kRoutes)
        if (path == r.path) route = &r;
    if (!route) {
        status = "404 Not Found";
        body = "<h1>404</h1>";
    } else {
        bool quit;
        std::string text = query_.run_command(route->command, quit);
        std::string nav;
        for (const Route& r : kRoutes) nav += std::string("<a href=\"") + r.path + "\">" + r.title + "</a> | ";
        body = "<!doctype html><html><head><meta charset=\"utf-8\"><meta http-equiv=\"refresh\" content=\"2\">"
               "<title>KADP-DSM</title><style>body{font-family:sans-serif;margin:1.5em}pre{background:#111;color:#ddd;"
               "padding:1em;overflow:auto}a{margin-right:.3em}</style></head><body><h2>KADP-DSM - " +
               std::string(route->title) + "</h2><p>" + nav + "</p><pre>" + html_escape(text) + "</pre></body></html>";
    }
    std::string resp = "HTTP/1.0 " + status + "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);
}
