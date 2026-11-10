#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "memory.h"

static int g_failed = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); g_failed++; } } while (0)

enum Op { READ, WRITE };

// Runs one access to the user view in a child process and returns the signal that killed it
// (0 if it finished normally). Shared mappings are inherited, so the child sees the same pages.
static int access_in_child(void* addr, Op op) {
    pid_t pid = fork();
    if (pid == 0) {
        volatile char* p = static_cast<volatile char*>(addr);
        if (op == READ) (void)*p; else *p = 1;
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) ? WTERMSIG(status) : 0;
}

int main() {
    Config cfg;  // defaults: 8 nodes, 1 MiB each, 4096-byte pages, base 0x600000000000
    Memory mem;
    CHECK(!mem.ready());
    CHECK(mem.init(cfg) == DSM_OK && mem.ready());
    CHECK(mem.init(cfg) == DSM_EINVAL);  // already initialised

    // The user view is exactly at base_address; the system view lives elsewhere.
    CHECK(mem.user_addr(0) == reinterpret_cast<void*>(cfg.base_address));
    CHECK(mem.sys_addr(0) != mem.user_addr(0));
    CHECK(mem.pages_per_node() == 256 && mem.total_pages() == 2048);

    // Section 3.2 example: base + 2 457 600 is page 600, home node 2, offset 88.
    void* a = reinterpret_cast<char*>(cfg.base_address) + 2457600;
    CHECK(mem.contains(a) && mem.page_of(a) == 600);
    CHECK(mem.home_of(600) == 2 && mem.offset_of(600) == 88 && mem.first_page_of(2) == 512);
    CHECK(mem.page_of(static_cast<char*>(a) + 4095) == 600);  // last byte of the same page
    CHECK(!mem.contains(reinterpret_cast<void*>(cfg.base_address - 1)));
    CHECK(!mem.contains(reinterpret_cast<char*>(cfg.base_address) + 8 * cfg.node_memory));
    CHECK(mem.valid_page(2047) && !mem.valid_page(2048));

    // A second Memory cannot take the same address: NOREPLACE refuses, reported as ECONFIG.
    {
        Memory other;
        CHECK(other.init(cfg) == DSM_ECONFIG && !other.ready());
    }

    // Local page table starts blank.
    CHECK(mem.entry(10).state == I && mem.entry(10).flags == 0 && mem.entry(10).waiters_head == NO_WAITER);
    CHECK(&mem.page_lock(5) == &mem.page_lock(5 + Memory::kLockStripes));
    CHECK(&mem.page_lock(5) != &mem.page_lock(6));
    mem.activate_home_range(1);
    CHECK(mem.entry(256).flags & PG_HOME && mem.entry(511).flags & PG_HOME);
    CHECK(!(mem.entry(255).flags & PG_HOME) && !(mem.entry(512).flags & PG_HOME));

    // State I: no permissions. Any access kills the process with SIGSEGV.
    const page_id_t p = 600;
    CHECK(access_in_child(mem.user_addr(p), READ) == SIGSEGV);
    CHECK(access_in_child(mem.user_addr(p), WRITE) == SIGSEGV);

    // Install through the system view as S: readable with the right contents, not writable.
    std::vector<char> data(mem.page_size());
    memset(data.data(), 0xAB, data.size());
    strcpy(data.data(), "hola dsm");
    CHECK(mem.install_page(p, data.data(), S) == DSM_OK);
    CHECK(memcmp(mem.user_addr(p), data.data(), data.size()) == 0);
    CHECK(access_in_child(mem.user_addr(p), READ) == 0);
    CHECK(access_in_child(mem.user_addr(p), WRITE) == SIGSEGV);

    // Upgrade to M: now writable; a write through the user view shows up in the system view.
    CHECK(mem.apply_prot(p, M) == DSM_OK);
    *static_cast<char*>(mem.user_addr(p)) = 'Z';
    std::vector<char> out(mem.page_size());
    CHECK(mem.copy_out(p, out.data()) == DSM_OK && out[0] == 'Z' && memcmp(out.data() + 1, data.data() + 1, data.size() - 1) == 0);

    // Back to I: access is revoked again, but the bytes stay in the system view.
    CHECK(mem.apply_prot(p, I) == DSM_OK);
    CHECK(access_in_child(mem.user_addr(p), READ) == SIGSEGV);
    CHECK(mem.copy_out(p, out.data()) == DSM_OK && out[0] == 'Z');

    // Argument checks.
    CHECK(mem.apply_prot(p, IS_P) == DSM_EINVAL);             // transitional state
    CHECK(mem.install_page(p, data.data(), I) == DSM_EINVAL);  // cannot install into I
    CHECK(mem.apply_prot(9999, M) == DSM_EINVAL);
    CHECK(mem.install_page(p, nullptr, S) == DSM_EINVAL);
    CHECK(mem.copy_out(9999, out.data()) == DSM_EINVAL);

    // After shutdown the address is free again and the object can be reused.
    mem.shutdown();
    CHECK(!mem.ready() && !mem.contains(a));
    CHECK(mem.init(cfg) == DSM_OK);
    CHECK(mem.entry(p).state == I && mem.entry(p).flags == 0);  // fresh table
    mem.shutdown();

    if (g_failed) { fprintf(stderr, "%d check(s) failed\n", g_failed); return 1; }
    puts("test_memory OK");
    return 0;
}
