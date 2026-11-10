#include "config.h"

#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>

#include "kadp_types.h"

namespace {

constexpr uint64_t kUserSpaceLimit = 0x7FFFFFFFFFFFULL;  // top of x86-64 user space

using Setter = std::function<bool(Config&, const std::string&, std::string&)>;

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

// A '#' starts a comment at the line start or after whitespace (the sample file has
// trailing comments such as "port_base = 50000 # ...").
std::string strip_comment(const std::string& s) {
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] == '#' && (i == 0 || isspace(static_cast<unsigned char>(s[i - 1]))))
            return s.substr(0, i);
    return s;
}

// Plain decimal, or hex when `hex_ok` and prefixed with 0x; optional K/M/G suffix.
bool parse_u64(const std::string& v, bool suffix_ok, bool hex_ok, uint64_t& out,
               std::string& why) {
    if (v.empty()) { why = "empty value"; return false; }
    int base = 10;
    std::string digits = v;
    uint64_t mult = 1;
    if (hex_ok && v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        base = 16;
        digits = v.substr(2);
    } else if (suffix_ok) {
        char last = static_cast<char>(toupper(static_cast<unsigned char>(v.back())));
        if (last == 'K' || last == 'M' || last == 'G') {
            mult = last == 'K' ? 1ULL << 10 : last == 'M' ? 1ULL << 20 : 1ULL << 30;
            digits = v.substr(0, v.size() - 1);
        }
    }
    if (digits.empty()) { why = "'" + v + "' is not a number"; return false; }
    for (char ch : digits)
        if (!isxdigit(static_cast<unsigned char>(ch)) || (base == 10 && !isdigit(static_cast<unsigned char>(ch)))) {
            why = "'" + v + "' is not a valid number";
            return false;
        }
    errno = 0;
    uint64_t n = strtoull(digits.c_str(), nullptr, base);
    if (errno == ERANGE || n > UINT64_MAX / mult) { why = "'" + v + "' is too large"; return false; }
    out = n * mult;
    return true;
}

bool in_range(uint64_t n, uint64_t lo, uint64_t hi, std::string& why) {
    if (n >= lo && n <= hi) return true;
    why = "value " + std::to_string(n) + " out of range " + std::to_string(lo) + ".." + std::to_string(hi);
    return false;
}

Setter u32_setter(uint32_t Config::*field, uint64_t lo, uint64_t hi, bool suffix = false) {
    return [=](Config& c, const std::string& v, std::string& why) {
        uint64_t n;
        if (!parse_u64(v, suffix, false, n, why) || !in_range(n, lo, hi, why)) return false;
        c.*field = static_cast<uint32_t>(n);
        return true;
    };
}

Setter u64_setter(uint64_t Config::*field, uint64_t lo, uint64_t hi, bool hex) {
    return [=](Config& c, const std::string& v, std::string& why) {
        uint64_t n;
        if (!parse_u64(v, !hex, hex, n, why)) return false;
        if (hex && v.rfind("0x", 0) != 0 && v.rfind("0X", 0) != 0) {
            why = "address must be hexadecimal (0x...)";
            return false;
        }
        if (!in_range(n, lo, hi, why)) return false;
        c.*field = n;
        return true;
    };
}

bool set_hosts(Config& c, const std::string& v, std::string& why) {
    std::vector<std::string> hosts;
    size_t start = 0;
    while (true) {
        size_t comma = v.find(',', start);
        std::string h = trim(v.substr(start, comma == std::string::npos ? comma : comma - start));
        if (h.empty() || h.size() > 253) { why = "empty or too long host entry"; return false; }
        for (char ch : h)
            if (!isalnum(static_cast<unsigned char>(ch)) && ch != '.' && ch != '-' && ch != '_') {
                why = "invalid character in host '" + h + "'";
                return false;
            }
        hosts.push_back(h);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (hosts.size() > 64) { why = "more than 64 hosts"; return false; }
    c.hosts = hosts;
    return true;
}

bool set_name(Config& c, const std::string& v, std::string& why) {
    if (v.empty() || v.size() > 31) { why = "name must have 1 to 31 characters"; return false; }
    c.cluster_name = v;
    return true;
}

bool set_log_dir(Config& c, const std::string& v, std::string& why) {
    if (v.empty()) { why = "empty path"; return false; }
    c.log_dir = v;
    return true;
}

bool set_log_level(Config& c, const std::string& v, std::string& why) {
    std::string up;
    for (char ch : v) up += static_cast<char>(toupper(static_cast<unsigned char>(ch)));
    if (up != "ERROR" && up != "WARN" && up != "INFO" && up != "DEBUG") {
        why = "must be ERROR, WARN, INFO or DEBUG";
        return false;
    }
    c.log_level = up;
    return true;
}

bool set_web(Config& c, const std::string& v, std::string& why) {
    if (v == "true") c.web_enabled = true;
    else if (v == "false") c.web_enabled = false;
    else { why = "must be true or false"; return false; }
    return true;
}

// "section.key" -> setter. Anything not listed here is an unknown parameter.
const std::map<std::string, Setter>& setters() {
    static const std::map<std::string, Setter> t = {
        {"cluster.name", set_name},
        {"cluster.hosts", set_hosts},
        {"cluster.max_nodes", u32_setter(&Config::max_nodes, 1, 64)},
        {"cluster.max_nodes_per_host", u32_setter(&Config::max_nodes_per_host, 1, 10)},
        {"memory.page_size", u32_setter(&Config::page_size, 1, 65536, true)},
        {"memory.node_memory", u64_setter(&Config::node_memory, 64ULL << 10, 1ULL << 30, false)},
        {"memory.base_address", u64_setter(&Config::base_address, 1, kUserSpaceLimit, true)},
        {"network.port_base", u32_setter(&Config::port_base, 1024, 65000)},
        {"network.port_stride", u32_setter(&Config::port_stride, 4, 100)},
        {"network.connect_timeout_ms", u32_setter(&Config::connect_timeout_ms, 100, 60000)},
        {"network.request_timeout_ms", u32_setter(&Config::request_timeout_ms, 100, 60000)},
        {"network.max_retries", u32_setter(&Config::max_retries, 0, 10)},
        {"network.connect_retry_ms", u32_setter(&Config::connect_retry_ms, 50, 10000)},
        {"heartbeat.interval_ms", u32_setter(&Config::hb_interval_ms, 100, 10000)},
        {"heartbeat.suspect_after", u32_setter(&Config::hb_suspect_after, 1, 20)},
        {"heartbeat.dead_after", u32_setter(&Config::hb_dead_after, 1, 1000)},
        {"cache.max_cached_pages", u32_setter(&Config::max_cached_pages, 0, UINT32_MAX)},
        {"log.dir", set_log_dir},
        {"log.level", set_log_level},
        {"web.enabled", set_web},
        {"extras.replication_factor", u32_setter(&Config::replication_factor, 0, 1)},
    };
    return t;
}

std::string where(const std::string& path, int line) {
    return line > 0 ? path + ":" + std::to_string(line) + ": " : path + ": ";
}

// Cross-parameter rules of section 8.3. `lines` maps "section.key" to its line so the
// message can point at the offending parameter (0 when it came from a default).
bool validate(const Config& c, const std::string& path, const std::map<std::string, int>& lines,
              std::string& err) {
    auto line_of = [&](const char* key) {
        auto it = lines.find(key);
        return it == lines.end() ? 0 : it->second;
    };
    auto fail = [&](const char* key, const std::string& msg) {
        err = where(path, line_of(key)) + key + ": " + msg;
        return false;
    };

    long sys_page = sysconf(_SC_PAGESIZE);
    if (sys_page <= 0 || c.page_size % static_cast<uint32_t>(sys_page) != 0)
        return fail("memory.page_size", "must be a multiple of the system page size (" +
                                            std::to_string(sys_page) + ")");
    if (c.node_memory % c.page_size != 0)
        return fail("memory.node_memory", "must be a multiple of page_size");
    if (c.base_address % c.page_size != 0)
        return fail("memory.base_address", "must be aligned to page_size");
    // base < limit first, so the sum below cannot overflow (max_nodes * node_memory <= 64 GiB).
    if (c.base_address >= kUserSpaceLimit ||
        c.base_address + static_cast<uint64_t>(c.max_nodes) * c.node_memory >= kUserSpaceLimit)
        return fail("memory.base_address",
                    "base_address + max_nodes * node_memory must stay below 0x7FFFFFFFFFFF");
    if (static_cast<uint64_t>(c.port_base) + static_cast<uint64_t>(c.port_stride) * c.max_nodes_per_host > 65535)
        return fail("network.port_base", "port_base + port_stride * max_nodes_per_host exceeds 65535");
    if (c.hb_dead_after <= c.hb_suspect_after)
        return fail("heartbeat.dead_after", "must be greater than suspect_after");
    return true;
}

}  // namespace

int config_load(const std::string& path, Config& out, std::string& err) {
    std::ifstream in(path);
    if (!in) {
        err = path + ": cannot open file";
        return DSM_ECONFIG;
    }

    Config c;
    std::map<std::string, int> lines;
    std::string section, raw;
    int lineno = 0;
    while (std::getline(in, raw)) {
        lineno++;
        std::string s = trim(strip_comment(raw));
        if (s.empty()) continue;

        if (s.front() == '[') {
            if (s.back() != ']') { err = where(path, lineno) + "malformed section header"; return DSM_ECONFIG; }
            section = trim(s.substr(1, s.size() - 2));
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos) { err = where(path, lineno) + "expected 'key = value'"; return DSM_ECONFIG; }
        if (section.empty()) { err = where(path, lineno) + "parameter outside of a section"; return DSM_ECONFIG; }

        std::string key = section + "." + trim(s.substr(0, eq));
        std::string value = trim(s.substr(eq + 1));
        auto it = setters().find(key);
        if (it == setters().end()) { err = where(path, lineno) + "unknown parameter '" + key + "'"; return DSM_ECONFIG; }

        std::string why;
        if (!it->second(c, value, why)) { err = where(path, lineno) + key + ": " + why; return DSM_ECONFIG; }
        lines[key] = lineno;
    }

    if (!validate(c, path, lines, err)) return DSM_ECONFIG;
    out = c;
    return DSM_OK;
}

uint32_t config_hash(const Config& c) {
    // FNV-1a over the fields in big-endian, so the value is the same on any machine.
    uint32_t h = 2166136261u;
    auto feed = [&h](uint64_t v, int bytes) {
        for (int i = bytes - 1; i >= 0; i--) {
            h ^= static_cast<uint8_t>(v >> (8 * i));
            h *= 16777619u;
        }
    };
    feed(c.page_size, 4);
    feed(c.node_memory, 8);
    feed(c.max_nodes, 4);
    feed(c.base_address, 8);
    return h;
}
