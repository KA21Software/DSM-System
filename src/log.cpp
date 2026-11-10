#include "log.h"

#include <sys/stat.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {
std::mutex g_mu;
FILE* g_file = nullptr;
LogLevel g_level = LOG_LVL_INFO;
const char* const kNames[] = {"ERROR", "WARN", "INFO", "DEBUG"};

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
}  // namespace

bool log_init(const std::string& dir, const std::string& level, const std::string& tag) {
    int lvl = -1;
    for (int i = 0; i < 4; i++)
        if (level == kNames[i]) lvl = i;
    if (lvl < 0) return false;

    mkdir(dir.c_str(), 0755);  // already existing is fine; fopen reports real failures
    FILE* f = fopen((dir + "/node-" + tag + ".log").c_str(), "a");
    if (!f) return false;
    setvbuf(f, nullptr, _IOLBF, 0);  // line-buffered so a crash loses nothing useful

    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file) fclose(g_file);
    g_file = f;
    g_level = static_cast<LogLevel>(lvl);
    return true;
}

void log_close() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file) fclose(g_file);
    g_file = nullptr;
}

bool log_enabled(LogLevel lvl) {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_file && lvl <= g_level;
}

void log_write(LogLevel lvl, const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_file || lvl > g_level) return;
    fprintf(g_file, "%lld [%s] %s\n", now_ms(), kNames[lvl], msg);
}
