#include "query.h"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

#include "log.h"
#include "stats.h"

namespace {

uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

const char* state_name(uint8_t s) {
    switch (s) {
        case I: return "I";
        case S: return "S";
        case M: return "M";
        case IS_P: return "IS_P";
        case IM_P: return "IM_P";
        case SM_P: return "SM_P";
        default: return "?";
    }
}

const char* node_state_name(uint8_t s) {
    switch (s) {
        case ALIVE: return "ALIVE";
        case SUSPECT: return "SUSPECT";
        case DEAD: return "DEAD";
        case LEFT: return "LEFT";
        default: return "?";
    }
}

std::string ip_string(uint32_t host_order) {
    return fmt("%u.%u.%u.%u", host_order >> 24, (host_order >> 16) & 255, (host_order >> 8) & 255, host_order & 255);
}

std::string copyset_string(uint64_t cs) {
    std::string s = "{";
    for (int i = 0; i < 64; i++)
        if (cs & (1ULL << i)) s += (s.size() > 1 ? "," : "") + std::to_string(i);
    return s + "}";
}

void write_stats(ByteWriter& w, const dsm_stats_t& s) {
    w.u16(s.node_id);
    w.u16(s.n_nodes_alive);
    w.u32(s.pages_home_total);
    w.u32(s.pages_home_used);
    w.u32(s.pages_cached_s);
    w.u32(s.pages_cached_m);
    w.u64(s.faults_read);
    w.u64(s.faults_write);
    w.u64(s.pages_sent);
    w.u64(s.pages_received);
    w.u64(s.invalidations_sent);
    w.u64(s.invalidations_received);
    w.u64(s.bytes_sent);
    w.u64(s.bytes_received);
    w.u32(s.avg_fault_us);
    w.u32(s.max_fault_us);
}

bool read_stats(ByteReader& r, dsm_stats_t& s) {
    memset(&s, 0, sizeof s);
    s.node_id = r.u16();
    s.n_nodes_alive = r.u16();
    s.pages_home_total = r.u32();
    s.pages_home_used = r.u32();
    s.pages_cached_s = r.u32();
    s.pages_cached_m = r.u32();
    s.faults_read = r.u64();
    s.faults_write = r.u64();
    s.pages_sent = r.u64();
    s.pages_received = r.u64();
    s.invalidations_sent = r.u64();
    s.invalidations_received = r.u64();
    s.bytes_sent = r.u64();
    s.bytes_received = r.u64();
    s.avg_fault_us = r.u32();
    s.max_fault_us = r.u32();
    return r.ok();
}

constexpr uint32_t kPagesPerRequest = 1500;  // 1500 rows * 28 B stays under KADP_MAX_PAYLOAD

}  // namespace

// ------------------------------------------------------------------ statistics

void Query::local_stats(dsm_stats_t& out) {
    memset(&out, 0, sizeof out);
    Counters& c = counters();
    out.node_id = ms_.my_id();
    for (const NodeInfo& n : ms_.snapshot())
        if (n.state == ALIVE || n.state == SUSPECT) out.n_nodes_alive++;
    out.pages_home_total = mem_.pages_per_node();
    out.pages_home_used = mem_.pages_per_node() - alloc_.free_pages();
    for (page_id_t p = 0; p < mem_.total_pages(); p++) {
        std::lock_guard<std::mutex> lk(mem_.page_lock(p));
        uint8_t st = mem_.entry(p).state;
        if (st == S) out.pages_cached_s++;
        else if (st == M) out.pages_cached_m++;
    }
    out.faults_read = c.faults_read.load();
    out.faults_write = c.faults_write.load();
    out.pages_sent = c.pages_sent.load();
    out.pages_received = c.pages_received.load();
    out.invalidations_sent = c.invalidations_sent.load();
    out.invalidations_received = c.invalidations_received.load();
    out.bytes_sent = data_net_.bytes_sent() + ms_.bytes_sent();
    out.bytes_received = data_net_.bytes_received() + ms_.bytes_received();
    uint64_t n = c.fault_count.load();
    out.avg_fault_us = n ? static_cast<uint32_t>(c.fault_us_total.load() / n) : 0;
    out.max_fault_us = static_cast<uint32_t>(c.fault_us_max.load());
}

int Query::node_stats(node_id_t node, dsm_stats_t& out) {
    if (node == ms_.my_id()) {
        local_stats(out);
        return DSM_OK;
    }
    Header h;
    h.type = STATS_REQUEST;
    ControlPlane::Reply rep;
    int rc = cp_.call(node, h, nullptr, 0, rep, cfg_.request_timeout_ms);
    if (rc != DSM_OK) return rc;
    ByteReader r(rep.payload.data(), rep.payload.size());
    return read_stats(r, out) ? DSM_OK : DSM_EPROTO;
}

int Query::global_stats(dsm_stats_t& out) {
    memset(&out, 0, sizeof out);
    out.node_id = ms_.my_id();
    uint64_t lat_total = 0, lat_n = 0;
    for (const NodeInfo& n : ms_.snapshot()) {
        if (n.state == DEAD || n.state == LEFT) continue;
        dsm_stats_t s;
        if (node_stats(n.node_id, s) != DSM_OK) continue;  // unreachable: not counted
        out.n_nodes_alive++;
        out.pages_home_total += s.pages_home_total;
        out.pages_home_used += s.pages_home_used;
        out.pages_cached_s += s.pages_cached_s;
        out.pages_cached_m += s.pages_cached_m;
        out.faults_read += s.faults_read;
        out.faults_write += s.faults_write;
        out.pages_sent += s.pages_sent;
        out.pages_received += s.pages_received;
        out.invalidations_sent += s.invalidations_sent;
        out.invalidations_received += s.invalidations_received;
        out.bytes_sent += s.bytes_sent;
        out.bytes_received += s.bytes_received;
        uint64_t faults = s.faults_read + s.faults_write;
        lat_total += static_cast<uint64_t>(s.avg_fault_us) * faults;
        lat_n += faults;
        out.max_fault_us = std::max(out.max_fault_us, s.max_fault_us);
    }
    out.avg_fault_us = lat_n ? static_cast<uint32_t>(lat_total / lat_n) : 0;
    return DSM_OK;
}

// ----------------------------------------------------------------- page tables

// One row per page that is worth listing: allocated here, cached here in some state, or
// busy in the directory. `everything` lists every page of the window (the `page` command).
std::vector<Query::PageRow> Query::collect_pages(uint32_t first, uint32_t count, bool everything) {
    std::vector<PageRow> rows;
    const node_id_t me = ms_.my_id();
    for (uint32_t p = first; p < first + count && p < mem_.total_pages() && rows.size() < kPagesPerRequest; p++) {
        PageRow row{};
        row.page = p;
        row.dir_state = kNoDir;
        row.owner = NODE_NONE;
        bool interesting;
        {
            std::lock_guard<std::mutex> lk(mem_.page_lock(p));
            const PageEntry& e = mem_.entry(p);
            row.local_state = e.state;
            row.version = e.version;
            row.alloc_id = e.alloc_id;
            row.alloc_order = e.alloc_order;
            interesting = e.state != I || (e.flags & PG_ALLOCATED);
            row.lost = (e.flags & PG_LOST) != 0;
        }
        if (mem_.home_of(p) == me) {
            DirEntry d;
            bool allocated, lost;
            if (dir_.entry_info(p, d, allocated, lost)) {
                if (allocated) {
                    row.dir_state = d.state;
                    row.owner = d.owner;
                    row.copyset = d.copyset;
                    row.busy = d.busy;
                    row.version = d.version;
                    interesting = true;
                }
                row.lost = row.lost || lost;
            }
        }
        if (interesting || everything) rows.push_back(row);
    }
    return rows;
}

int Query::remote_pages(node_id_t node, uint32_t first, uint32_t count, bool everything, std::vector<PageRow>& out) {
    if (node == ms_.my_id()) {
        out = collect_pages(first, count, everything);
        return DSM_OK;
    }
    std::vector<uint8_t> payload;
    ByteWriter w(payload);
    w.u32(first);
    w.u32(count);
    w.u8(everything ? 1 : 0);
    Header h;
    h.type = PAGE_TABLE_REQUEST;
    ControlPlane::Reply rep;
    int rc = cp_.call(node, h, payload.data(), payload.size(), rep, cfg_.request_timeout_ms);
    if (rc != DSM_OK) return rc;
    ByteReader r(rep.payload.data(), rep.payload.size());
    uint16_t n = r.u16();
    for (uint16_t i = 0; i < n; i++) {
        PageRow row{};
        row.page = r.u32();
        row.local_state = r.u8();
        row.dir_state = r.u8();
        uint8_t flags = r.u8();
        row.busy = flags & 1;
        row.lost = (flags >> 1) & 1;
        row.owner = r.u16();
        row.version = r.u32();
        row.alloc_id = r.u32();
        row.alloc_order = r.u32();
        row.copyset = r.u64();
        if (!r.ok()) return DSM_EPROTO;
        out.push_back(row);
    }
    return DSM_OK;
}

// ----------------------------------------------------- answering other nodes

void Query::on_message(const std::shared_ptr<Connection>& conn, const Header& h, const uint8_t* payload) {
    switch (h.type) {
        case STATS_REQUEST: {
            dsm_stats_t s;
            local_stats(s);
            std::vector<uint8_t> out;
            ByteWriter w(out);
            write_stats(w, s);
            cp_.reply(conn, h, STATS_REPLY, DSM_OK, out.data(), out.size());
            break;
        }
        case PAGE_TABLE_REQUEST: {
            ByteReader r(payload, h.payload_len);
            uint32_t first = r.u32(), count = r.u32();
            bool everything = r.u8() != 0;
            if (!r.ok()) return cp_.reply(conn, h, PAGE_TABLE_REPLY, DSM_EPROTO);
            auto rows = collect_pages(first, count, everything);
            std::vector<uint8_t> out;
            ByteWriter w(out);
            w.u16(static_cast<uint16_t>(rows.size()));
            for (const PageRow& row : rows) {
                w.u32(row.page);
                w.u8(row.local_state);
                w.u8(row.dir_state);
                w.u8(static_cast<uint8_t>((row.busy ? 1 : 0) | (row.lost ? 2 : 0)));
                w.u16(row.owner);
                w.u32(row.version);
                w.u32(row.alloc_id);
                w.u32(row.alloc_order);
                w.u64(row.copyset);
            }
            cp_.reply(conn, h, PAGE_TABLE_REPLY, DSM_OK, out.data(), out.size());
            break;
        }
        case ALLOCS_REQUEST: {
            auto pieces = alloc_.pieces();
            if (pieces.size() > 900) pieces.resize(900);  // stays under the payload limit
            std::vector<uint8_t> out;
            ByteWriter w(out);
            w.u16(static_cast<uint16_t>(pieces.size()));
            for (const auto& p : pieces) {
                w.u32(p.rec.alloc_id);
                w.u16(p.rec.owner_node);
                w.u32(static_cast<uint32_t>(p.rec.owner_pid));
                w.u32(p.rec.first_page);
                w.u32(p.rec.n_pages);
                w.u64(p.rec.size_bytes);
                w.u32(p.alloc_start);
                w.bytes(p.rec.name, sizeof p.rec.name);
            }
            cp_.reply(conn, h, ALLOCS_REPLY, DSM_OK, out.data(), out.size());
            break;
        }
        default:
            break;
    }
}

// --------------------------------------------------------------------- commands

std::string Query::cmd_nodes() {
    std::string out = " ID DIRECCION          ESTADO   APORTE     LIBRE      ULT.HB\n";
    node_id_t coord = ms_.coordinator_id();
    uint64_t now = now_ms();
    for (const NodeInfo& n : ms_.snapshot()) {
        bool me = n.node_id == ms_.my_id();
        uint32_t freep = me ? alloc_.free_pages() : n.free_pages;
        double age = me ? 0.0 : static_cast<double>(now - n.last_hb_ms) / 1000.0;
        out += fmt("%3u %-18s %-8s %6llu KB %6llu KB %5.1f s%s%s\n", n.node_id,
                   (ip_string(n.ipv4) + ":" + std::to_string(n.slot)).c_str(), node_state_name(n.state),
                   static_cast<unsigned long long>(n.mem_pages) * cfg_.page_size / 1024,
                   static_cast<unsigned long long>(freep) * cfg_.page_size / 1024, age,
                   n.node_id == coord ? " (coordinador)" : "", me ? " (este nodo)" : "");
    }
    return out;
}

static std::string stats_text(const char* title, const dsm_stats_t& s) {
    std::string o = fmt("%s\n", title);
    o += fmt("  nodos vivos          %u\n", s.n_nodes_alive);
    o += fmt("  paginas home         %u usadas de %u\n", s.pages_home_used, s.pages_home_total);
    o += fmt("  paginas en cache     %u en S, %u en M\n", s.pages_cached_s, s.pages_cached_m);
    o += fmt("  fallos               %llu de lectura, %llu de escritura\n", static_cast<unsigned long long>(s.faults_read),
             static_cast<unsigned long long>(s.faults_write));
    o += fmt("  paginas              %llu enviadas, %llu recibidas\n", static_cast<unsigned long long>(s.pages_sent),
             static_cast<unsigned long long>(s.pages_received));
    o += fmt("  invalidaciones       %llu enviadas, %llu recibidas\n", static_cast<unsigned long long>(s.invalidations_sent),
             static_cast<unsigned long long>(s.invalidations_received));
    o += fmt("  bytes                %llu enviados, %llu recibidos\n", static_cast<unsigned long long>(s.bytes_sent),
             static_cast<unsigned long long>(s.bytes_received));
    o += fmt("  latencia de fallo    %u us promedio, %u us maxima\n", s.avg_fault_us, s.max_fault_us);
    return o;
}

std::string Query::cmd_stats(const std::string& arg) {
    dsm_stats_t s;
    if (arg.empty()) {
        local_stats(s);
        return stats_text("Estadisticas de este nodo", s);
    }
    if (arg == "global") {
        global_stats(s);
        return stats_text("Estadisticas globales (suma de los nodos vivos)", s);
    }
    char* end;
    long id = strtol(arg.c_str(), &end, 10);
    if (*end || id < 0) return "uso: stats [global | <id>]\n";
    int rc = node_stats(static_cast<node_id_t>(id), s);
    if (rc != DSM_OK) return fmt("nodo %ld: %s\n", id, dsm_strerror(rc));
    return stats_text(fmt("Estadisticas del nodo %ld", id).c_str(), s);
}

std::string Query::cmd_pages_local() {
    std::string out = " PAGINA ESTADO VERSION ALLOC      ORDEN HOME\n";
    int n = 0;
    for (const PageRow& r : collect_pages(0, mem_.total_pages(), false)) {
        if (r.local_state == I && r.alloc_id == 0) continue;
        out += fmt("%7u %-6s %7u 0x%08x %5u %4u%s\n", r.page, state_name(r.local_state), r.version, r.alloc_id,
                   r.alloc_order, mem_.home_of(r.page), r.lost ? "  LOST" : "");
        n++;
    }
    return out + fmt("(%d paginas)\n", n);
}

std::string Query::cmd_pages_global() {
    std::string out = " PAGINA HOME ESTADO     DUENO COPIAS         VERSION ALLOC\n";
    int n = 0;
    for (const NodeInfo& node : ms_.snapshot()) {
        if (node.state == DEAD || node.state == LEFT) {
            out += fmt("  (nodo %u: %s, sus paginas se perdieron)\n", node.node_id, node_state_name(node.state));
            continue;
        }
        page_id_t first = mem_.first_page_of(node.node_id);
        for (uint32_t off = 0; off < mem_.pages_per_node(); off += kPagesPerRequest) {
            std::vector<PageRow> rows;
            uint32_t count = std::min(kPagesPerRequest, mem_.pages_per_node() - off);
            if (remote_pages(node.node_id, first + off, count, false, rows) != DSM_OK) {
                out += fmt("  (nodo %u no respondio)\n", node.node_id);
                break;
            }
            for (const PageRow& r : rows) {
                if (r.dir_state == kNoDir) continue;  // cached there but not allocated: not a directory row
                out += fmt("%7u %4u %-10s %5s %-14s %7u 0x%08x%s\n", r.page, node.node_id,
                           r.dir_state == D_MODIFIED ? "MODIFIED" : "SHARED",
                           r.owner == NODE_NONE ? "-" : std::to_string(r.owner).c_str(), copyset_string(r.copyset).c_str(),
                           r.version, r.alloc_id, r.busy ? " (ocupada)" : "");
                n++;
            }
        }
    }
    return out + fmt("(%d paginas asignadas)\n", n);
}

std::string Query::cmd_page(const std::string& arg) {
    char* end;
    long page = strtol(arg.c_str(), &end, 10);
    if (arg.empty() || *end || page < 0 || !mem_.valid_page(static_cast<page_id_t>(page))) return "uso: page <page_id>\n";
    node_id_t home = mem_.home_of(static_cast<page_id_t>(page));
    std::string out = fmt("Pagina %ld, home en el nodo %u\n NODO ESTADO LOCAL VERSION\n", page, home);
    std::string dir;
    for (const NodeInfo& node : ms_.snapshot()) {
        if (node.state == DEAD || node.state == LEFT) {
            out += fmt("%5u %s\n", node.node_id, node_state_name(node.state));
            continue;
        }
        std::vector<PageRow> rows;
        int rc = remote_pages(node.node_id, static_cast<uint32_t>(page), 1, true, rows);
        if (rc != DSM_OK || rows.empty()) {
            out += fmt("%5u %s\n", node.node_id, rc == DSM_OK ? "sin datos" : dsm_strerror(rc));
            continue;
        }
        const PageRow& r = rows[0];
        out += fmt("%5u %-12s %7u%s\n", node.node_id, state_name(r.local_state), r.version, r.lost ? "  LOST" : "");
        if (node.node_id == home && r.dir_state != kNoDir)
            dir = fmt("Directorio: %s, dueno %s, copias %s, version %u%s\n", r.dir_state == D_MODIFIED ? "MODIFIED" : "SHARED",
                      r.owner == NODE_NONE ? "-" : std::to_string(r.owner).c_str(), copyset_string(r.copyset).c_str(),
                      r.version, r.busy ? ", ocupada" : "");
        else if (node.node_id == home)
            dir = "Directorio: la pagina no esta asignada\n";
    }
    return out + dir;
}

std::string Query::cmd_allocs() {
    struct Row {
        uint32_t alloc_id, start;
        uint16_t owner;
        int32_t pid;
        uint64_t size;
        char name[25];
        std::vector<std::pair<uint32_t, uint32_t>> pieces;  // (first page, n pages)
    };
    std::map<uint32_t, Row> rows;
    auto add = [&](const Allocator::Piece& p) {
        Row& r = rows[p.rec.alloc_id];
        r.alloc_id = p.rec.alloc_id;
        r.start = p.alloc_start;
        r.owner = p.rec.owner_node;
        r.pid = p.rec.owner_pid;
        r.size = p.rec.size_bytes;
        memcpy(r.name, p.rec.name, 24);
        r.name[24] = 0;
        r.pieces.push_back({p.rec.first_page, p.rec.n_pages});
    };
    for (const auto& p : alloc_.pieces()) add(p);
    for (const NodeInfo& node : ms_.snapshot()) {
        if (node.node_id == ms_.my_id() || node.state == DEAD || node.state == LEFT) continue;
        Header h;
        h.type = ALLOCS_REQUEST;
        ControlPlane::Reply rep;
        if (cp_.call(node.node_id, h, nullptr, 0, rep, cfg_.request_timeout_ms) != DSM_OK) continue;
        ByteReader r(rep.payload.data(), rep.payload.size());
        uint16_t n = r.u16();
        for (uint16_t i = 0; i < n && r.ok(); i++) {
            Allocator::Piece p{};
            p.rec.alloc_id = r.u32();
            p.rec.owner_node = r.u16();
            p.rec.owner_pid = static_cast<int32_t>(r.u32());
            p.rec.first_page = r.u32();
            p.rec.n_pages = r.u32();
            p.rec.size_bytes = r.u64();
            p.alloc_start = r.u32();
            r.bytes(p.rec.name, sizeof p.rec.name);
            if (r.ok()) add(p);
        }
    }
    std::string out = " ALLOC      NOMBRE               PROCESO       PAGINAS (en orden)            TAMANO\n";
    for (auto& kv : rows) {
        Row& r = kv.second;
        std::sort(r.pieces.begin(), r.pieces.end());
        std::string pages;
        for (auto& pc : r.pieces) {
            if (!pages.empty()) pages += ", ";
            pages += fmt("%u-%u (home %u)", pc.first, pc.first + pc.second - 1, mem_.home_of(pc.first));
        }
        out += fmt("0x%08x %-20s n%u/pid %-6d %-30s %llu B\n", r.alloc_id, r.name, r.owner, r.pid, pages.c_str(),
                   static_cast<unsigned long long>(r.size));
    }
    return out + fmt("(%zu asignaciones)\n", rows.size());
}

std::string Query::cmd_names() {
    std::string out = " NOMBRE                           ALLOC      PAGINA   TAMANO       CREADOR\n";
    auto all = names_.all();
    for (const NameEntry& e : all)
        out += fmt(" %-32s 0x%08x %6u   %-12llu n%u\n", e.name, e.alloc_id, e.first_page,
                   static_cast<unsigned long long>(e.size_bytes), e.owner_node);
    return out + fmt("(%zu nombres)\n", all.size());
}

std::string Query::run_command(const std::string& line, bool& quit) {
    quit = false;
    std::istringstream in(line);
    std::string cmd, a1, a2;
    in >> cmd >> a1 >> a2;
    if (cmd.empty()) return "";
    if (cmd == "help")
        return "Comandos:\n"
               "  help                 esta lista\n"
               "  nodes                tabla de nodos\n"
               "  stats [global|<id>]  estadisticas del nodo, de otro nodo o de todos\n"
               "  pages local          paginas de este nodo (estado, version, asignacion)\n"
               "  pages global         directorios de todos los homes\n"
               "  page <page_id>       una pagina en todos los nodos\n"
               "  allocs               asignaciones\n"
               "  names                nombres\n"
               "  quit                 salir del nodo\n";
    if (cmd == "nodes") return cmd_nodes();
    if (cmd == "stats") return cmd_stats(a1);
    if (cmd == "pages") {
        if (a1 == "local") return cmd_pages_local();
        if (a1 == "global") return cmd_pages_global();
        return "uso: pages local | pages global\n";
    }
    if (cmd == "page") return cmd_page(a1);
    if (cmd == "allocs") return cmd_allocs();
    if (cmd == "names") return cmd_names();
    if (cmd == "quit" || cmd == "exit") {
        quit = true;
        return "saliendo...\n";
    }
    return "comando desconocido: " + cmd + " (escriba help)\n";
}

// ---------------------------------------------------------------------- console

void Query::start_console(std::function<void()> on_quit) {
    if (console_running_.exchange(true)) return;
    on_quit_ = std::move(on_quit);
    console_ = std::thread(&Query::console_loop, this);
}

void Query::stop_console() {
    console_running_ = false;
    if (!console_.joinable()) return;
    // `quit` runs the shutdown from the console thread itself, which cannot join itself.
    if (console_.get_id() == std::this_thread::get_id()) console_.detach();
    else console_.join();
}

void Query::console_loop() {
    const bool tty = isatty(STDIN_FILENO);
    std::string prompt = fmt("kadp[n%u]> ", ms_.my_id());
    std::string buf;
    if (tty) fputs(prompt.c_str(), stdout), fflush(stdout);
    while (console_running_.load()) {
        pollfd p{STDIN_FILENO, POLLIN, 0};
        int rc = poll(&p, 1, 200);  // short, so stop_console() is noticed
        if (rc <= 0) continue;
        char chunk[256];
        ssize_t n = read(STDIN_FILENO, chunk, sizeof chunk);
        if (n <= 0) break;  // EOF: no console (stdin is /dev/null, or the pipe closed)
        buf.append(chunk, static_cast<size_t>(n));
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            bool quit = false;
            std::string text = run_command(line, quit);
            fputs(text.c_str(), stdout);
            if (quit) {
                fflush(stdout);
                console_running_ = false;
                if (on_quit_) on_quit_();
                return;
            }
            if (tty) fputs(prompt.c_str(), stdout);
            fflush(stdout);
        }
    }
}
