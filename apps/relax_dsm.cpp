// Relaxation technique on the DSM: every interior cell is replaced by the average of its four
// neighbours until no cell changes by more than the precision.
//
// Adapted from src/shared_architecture/main.c of
// github.com/Adamouization/Relaxation-Technique-Parallel-Computing
// (MIT License, (c) 2018 Adam Jaamour; license text in apps/LICENSE-relaxation).
// What stays the same: the update rule of relaxation_runner() (average of left, right, up and
// down), the fixed boundary values and the stop test "all updates differ by less than the
// precision". What changes:
//   - a "thread" is now a node: each node relaxes its own block of rows (the original makes
//     every thread sweep the whole array);
//   - the array is a pair of named DSM allocations (dsm_alloc_named) instead of malloc'ed
//     rows of pointers, and the update is double buffered (Jacobi) so the result does not
//     depend on the order of the updates and can be checked against a sequential run;
//   - the per-cell pthread mutexes are gone: one dsm_barrier per iteration separates the
//     phases, and the convergence test reduces one value per node through shared memory;
//   - rand() is replaced by a hash of (row, column), so every node can fill its own rows.
//
// Usage: relax_dsm <dsm.conf> <dim> <precision> <max iterations> <print 0|1> <nodes>
// Start it once per node, node 0 first.
#include <sys/time.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include "dsm.h"

namespace {

long dim, maxIter, numOfNodes;
double precision;
int me;
double *gridA, *gridB;  // the two buffers
double* diffs;          // one page per node and per parity: this node's largest update

constexpr size_t kPage = 4096;

// Deterministic stand-in for (double)(rand() % 100): a value in [0, 100) from (row, column).
double element(long i, long j) {
    uint64_t x = (static_cast<uint64_t>(i) << 32) ^ static_cast<uint64_t>(j);
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return static_cast<double>(x % 100);
}

void barrier(uint32_t id) {
    int rc = dsm_barrier(id, static_cast<uint16_t>(numOfNodes));
    if (rc != 0) {
        fprintf(stderr, "[node %d] barrier %u failed: %s\n", me, id, dsm_strerror(rc));
        exit(3);
    }
}

// Rows [start, end) of `rows` for worker `id` of `workers`; the first workers get one row
// fewer when the division is not exact (same rule as mm_dsm and the original mm_pthreads).
void partition(long rows, long id, long workers, long& start, long& end) {
    long base = rows / workers, remainder = rows % workers;
    if (id < workers - remainder) {
        start = id * base;
        end = start + base;
    } else {
        start = id * base + id - workers + remainder;
        end = start + base + 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 7) {
        printf("USAGE: %s <dsm.conf> <dim> <precision> <max iterations> <print 0|1> <nodes>\n", argv[0]);
        return 1;
    }
    char* end;
    dim = strtol(argv[2], &end, 10);
    if (*end || dim < 3) { printf("Invalid dimension (at least 3).\n"); return 1; }
    precision = strtod(argv[3], &end);
    if (*end || precision <= 0) { printf("Invalid precision.\n"); return 1; }
    maxIter = strtol(argv[4], &end, 10);
    if (*end || maxIter < 1) { printf("Invalid number of iterations.\n"); return 1; }
    long printResults = strtol(argv[5], &end, 10);
    if (*end) { printf("Invalid print flag.\n"); return 1; }
    numOfNodes = strtol(argv[6], &end, 10);
    if (*end || numOfNodes < 1) { printf("Invalid number of nodes.\n"); return 1; }

    int rc = dsm_init(argv[1]);
    if (rc != 0) { fprintf(stderr, "dsm_init: %s\n", dsm_strerror(rc)); return 1; }
    me = dsm_node_id();
    if (me >= numOfNodes) { fprintf(stderr, "node id %d is outside the %ld nodes of this run\n", me, numOfNodes); dsm_shutdown(); return 2; }
    printf("[node %d] up, waiting for %ld nodes...\n", me, numOfNodes);
    while (dsm_num_nodes() < numOfNodes) usleep(100000);

    // Every node asks for the same names; the first one creates them, the rest get the same pages.
    size_t grid_bytes = static_cast<size_t>(dim) * dim * sizeof(double);
    gridA = static_cast<double*>(dsm_alloc_named("relax_a", grid_bytes));
    gridB = static_cast<double*>(dsm_alloc_named("relax_b", grid_bytes));
    diffs = static_cast<double*>(dsm_alloc_named("relax_diffs", 2 * numOfNodes * kPage));
    if (!gridA || !gridB || !diffs) {
        fprintf(stderr, "[node %d] cannot allocate the arrays: %s (raise node_memory or max_nodes in dsm.conf)\n", me,
                dsm_strerror(dsm_last_error()));
        dsm_shutdown();
        return 2;
    }

    // Each node fills its own rows, boundaries included, in both buffers: the boundary
    // never changes, so the buffer that is not being written must hold it too.
    long s, e;
    partition(dim, me, numOfNodes, s, e);
    for (long i = s; i < e; i++)
        for (long j = 0; j < dim; j++) gridA[i * dim + j] = gridB[i * dim + j] = element(i, j);
    barrier(1);

    long is, ie;
    partition(dim - 2, me, numOfNodes, is, ie);  // interior rows 1..dim-2
    printf("[node %d] interior rows %ld..%ld of %ld, grid %ldx%ld ready\n", me, is + 1, ie, dim - 2, dim, dim);

    // Jacobi sweeps. Reads go to `cur`, writes to `nxt`; one barrier per sweep means nobody
    // starts the next sweep (which overwrites the old `cur`) while a neighbour still reads it.
    // Each node publishes its largest update in its own page of `diffs`, on a parity that
    // alternates, so reading everybody's value never races with the next sweep's write.
    double *cur = gridA, *nxt = gridB;
    const size_t per_page = kPage / sizeof(double);
    struct timeval t0, t1;
    gettimeofday(&t0, nullptr);
    long iter = 0;
    bool converged = false;
    while (iter < maxIter && !converged) {
        double local = 0;
        for (long i = is + 1; i <= ie; i++)
            for (long j = 1; j < dim - 1; j++) {
                double v = (cur[i * dim + j - 1] + cur[i * dim + j + 1] + cur[(i - 1) * dim + j] + cur[(i + 1) * dim + j]) / 4.0;
                double d = std::fabs(v - cur[i * dim + j]);
                if (d > local) local = d;
                nxt[i * dim + j] = v;
            }
        double* mine = diffs + (static_cast<size_t>(iter & 1) * numOfNodes + me) * per_page;
        *mine = local;
        barrier(10 + static_cast<uint32_t>(iter & 1));

        double global = 0;
        for (long n = 0; n < numOfNodes; n++) {
            double v = diffs[(static_cast<size_t>(iter & 1) * numOfNodes + n) * per_page];
            if (v > global) global = v;
        }
        converged = global < precision;
        std::swap(cur, nxt);
        iter++;
    }
    gettimeofday(&t1, nullptr);
    double elapsed = static_cast<double>(t1.tv_sec - t0.tv_sec) + static_cast<double>(t1.tv_usec - t0.tv_usec) / 1e6;
    printf("[node %d] %ld iterations (%s), time %f sec\n", me, iter, converged ? "converged" : "iteration limit", elapsed);

    // Node 0 repeats the same sweeps on one private array and compares every cell: the sweep
    // is deterministic (same operations in the same order), so the results must be identical.
    int ok = 1;
    if (me == 0) {
        std::vector<double> a(dim * dim), b(dim * dim);
        for (long i = 0; i < dim; i++)
            for (long j = 0; j < dim; j++) a[i * dim + j] = b[i * dim + j] = element(i, j);
        for (long it = 0; it < iter; it++) {
            for (long i = 1; i < dim - 1; i++)
                for (long j = 1; j < dim - 1; j++)
                    b[i * dim + j] = (a[i * dim + j - 1] + a[i * dim + j + 1] + a[(i - 1) * dim + j] + a[(i + 1) * dim + j]) / 4.0;
            a.swap(b);
        }
        long bad = 0;
        for (long i = 0; i < dim * dim; i++)
            if (a[i] != cur[i]) bad++;
        ok = bad == 0;
        printf("[node 0] verification against a sequential run (%ld cells): %s\n", dim * dim, ok ? "OK" : "FAIL");
        if (!ok) printf("[node 0] %ld cells differ\n", bad);
        if (printResults != 0)
            for (long i = 0; i < dim; i++) {
                for (long j = 0; j < dim; j++) printf("%9.4f", cur[i * dim + j]);
                printf("\n");
            }
    }
    barrier(3);  // nobody leaves while node 0 still reads pages that live on other nodes
    printf("[node %d] %s\n", me, ok ? "RESULT: OK" : "RESULT: FAIL");
    dsm_shutdown();
    return ok ? 0 : 1;
}
