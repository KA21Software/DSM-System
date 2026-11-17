// A node that does nothing but stay in the cluster, for demos of the console, of membership
// (a node killed or leaving) and of the lock cleanup after a node dies.
//
// Usage: idle_dsm <dsm.conf> <seconds> [plain|holder|waiter]
//   plain   just stays (default)
//   holder  takes dsm_lock(5) and keeps it until it exits or is killed
//   alloc   creates the named block "demo" (5000 bytes) and writes to it
//   waiter  after 2 s asks for dsm_lock(5); prints "GOT LOCK" when it gets it
// With a terminal on stdin (or DSM_CONSOLE=1) the console commands are available.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dsm.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <dsm.conf> <seconds> [plain|holder|waiter]\n", argv[0]);
        return 1;
    }
    const char* role = argc > 3 ? argv[3] : "plain";
    if (dsm_init(argv[1]) != 0) return 1;
    int me = dsm_node_id();
    printf("[node %d] up (%s)\n", me, role);
    fflush(stdout);

    if (!strcmp(role, "holder")) {
        int rc = dsm_lock(5);
        printf("[node %d] holder: lock 5 -> %s\n", me, dsm_strerror(rc));
        fflush(stdout);
    }
    if (!strcmp(role, "alloc")) {
        char* p = static_cast<char*>(dsm_alloc_named("demo", 5000));
        if (p) strcpy(p, "hello from the DSM");
        printf("[node %d] alloc: demo at %p\n", me, static_cast<void*>(p));
        fflush(stdout);
    }
    if (!strcmp(role, "waiter")) {
        sleep(2);
        int rc = dsm_lock(5);
        printf("[node %d] waiter: %s\n", me, rc == 0 ? "GOT LOCK" : dsm_strerror(rc));
        fflush(stdout);
    }
    for (int t = 0; t < atoi(argv[2]); t++) sleep(1);
    dsm_shutdown();
    printf("[node %d] bye\n", me);
    return 0;
}
