#include <cstdio>
#include <cstdlib>
#include <string>

#include "log.h"

static int count_lines(const std::string& path, const char* needle) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return -1;
    char buf[1024];
    int n = 0;
    while (fgets(buf, sizeof buf, f))
        if (std::string(buf).find(needle) != std::string::npos) n++;
    fclose(f);
    return n;
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main() {
    const std::string dir = "build/test_logs";
    remove((dir + "/node-t.log").c_str());

    CHECK(!log_init(dir, "VERBOSE", "t"));  // unknown level rejected
    CHECK(log_init(dir, "INFO", "t"));

    LOG_ERROR("seq=%u page_id=%u error", 7u, 42u);
    LOG_INFO("info visible");
    LOG_DEBUG("debug hidden");
    log_close();

    const std::string path = dir + "/node-t.log";
    CHECK(count_lines(path, "[ERROR] seq=7 page_id=42") == 1);
    CHECK(count_lines(path, "[INFO] info visible") == 1);
    CHECK(count_lines(path, "debug hidden") == 0);  // filtered by level

    puts("test_log OK");
    return 0;
}
