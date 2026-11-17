// Dense matrix multiplication C = A * B on the DSM.
//
// Adapted from mm_pthreads.c of github.com/dimitrisstyl7/parallel-computing-project
// (MIT License, (c) 2024 Dimitris Stylianou, Apostolis Siampanis, Konstantinos Loizidis;
// license text in apps/LICENSE-mm_pthreads). What stays the same: the command-line meaning of
// N, K, M, the row partition among workers and the triple loop of matricesMultiplication().
// What changes:
//   - a "thread" is now a node: worker id = DSM node id, numOfThreads = number of nodes;
//   - A, B and C are named allocations of the DSM (dsm_alloc_named) instead of malloc'ed
//     memory, so every node sees the same pages and they spread over the homes of every node;
//   - drand48() is replaced by a hash of (matrix, row, column): every node can then fill its
//     own slice, and any entry can be recomputed independently to verify the result;
//   - phases are separated with dsm_barrier, run by the coordinator of the cluster.
//
// Usage: mm_dsm <dsm.conf> <N> <K> <M> <print 0|1> <nodes>   (A is NxK, B is KxM, C is NxM)
// Start it once per node, node 0 first.
#include <sys/time.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "dsm.h"

namespace {

double *A, *B, *C;
long N, K, M, numOfNodes;
int me;

// Deterministic stand-in for 3.0 * drand48(): value in [0, 3) from (matrix id, row, col).
double element(int matrix, long i, long j) {
    uint64_t x = (static_cast<uint64_t>(matrix) << 56) ^ (static_cast<uint64_t>(i) << 28) ^ static_cast<uint64_t>(j);
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return 3.0 * static_cast<double>(x >> 11) / 9007199254740992.0;
}

void barrier(uint32_t phase) {
    int rc = dsm_barrier(phase, static_cast<uint16_t>(numOfNodes));
    if (rc != 0) {
        fprintf(stderr, "[node %d] barrier %u failed: %s\n", me, phase, dsm_strerror(rc));
        exit(3);
    }
}

// Same partition as the original: the first (numOfThreads - remainder) workers get one row
// fewer than the rest.
void partition(long rows, long id, long workers, long& start, long& end) {
    long numOfElements = rows / workers, remainder = rows % workers;
    if (id < workers - remainder) {
        start = id * numOfElements;
        end = start + numOfElements;
    } else {
        start = id * numOfElements + id - workers + remainder;
        end = start + numOfElements + 1;
    }
}

void matricesMultiplication(int id) {
    long start, end;
    partition(N, id, numOfNodes, start, end);
    printf("Node %d: start = %ld, end = %ld\n", id, start, end);

    // Matrix-Matrix multiplication (unchanged from the original)
    for (long i = start; i < end; i++)
        for (long k = 0; k < K; k++)
            for (long j = 0; j < M; j++)
                C[i * M + j] += A[i * K + k] * B[k * M + j];
}

// Recomputes `samples` entries of C from the generator and compares them.
bool verify(int samples) {
    int bad = 0;
    for (int s = 0; s < samples; s++) {
        long i = static_cast<long>(element(7, s, 1) / 3.0 * N) % N;
        long j = static_cast<long>(element(7, s, 2) / 3.0 * M) % M;
        double ref = 0;
        for (long k = 0; k < K; k++) ref += element(1, i, k) * element(2, k, j);
        double got = C[i * M + j];
        if (std::fabs(got - ref) > 1e-9 * (1.0 + std::fabs(ref))) {
            if (bad++ < 5) printf("MISMATCH C[%ld][%ld]: got %.12g, expected %.12g\n", i, j, got, ref);
        }
    }
    return bad == 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 7) {
        printf("USAGE: %s <dsm.conf> <N> <K> <M> <PRINT RESULTS> <NUMBER OF NODES>\n", argv[0]);
        return 1;
    }
    char* end;
    N = strtol(argv[2], &end, 10);
    if (*end || N < 1) { printf("Invalid number for N provided.\n"); return 1; }
    K = strtol(argv[3], &end, 10);
    if (*end || K < 1) { printf("Invalid number for K provided.\n"); return 1; }
    M = strtol(argv[4], &end, 10);
    if (*end || M < 1) { printf("Invalid number for M provided.\n"); return 1; }
    long printResults = strtol(argv[5], &end, 10);
    if (*end) { printf("Invalid number for printResults provided.\n"); return 1; }
    numOfNodes = strtol(argv[6], &end, 10);
    if (*end || numOfNodes < 1) { printf("Invalid number of nodes provided.\n"); return 1; }

    int rc = dsm_init(argv[1]);
    if (rc != 0) { fprintf(stderr, "dsm_init: %s\n", dsm_strerror(rc)); return 1; }
    me = dsm_node_id();
    if (me >= numOfNodes) { fprintf(stderr, "node id %d is outside the %ld nodes of this run\n", me, numOfNodes); dsm_shutdown(); return 2; }

    printf("[node %d] up, waiting for %ld nodes...\n", me, numOfNodes);
    while (dsm_num_nodes() < numOfNodes) usleep(100000);

    // Every node asks for the same names; the first one creates them, the rest get the same pages.
    A = static_cast<double*>(dsm_alloc_named("A", N * K * sizeof(double)));
    B = static_cast<double*>(dsm_alloc_named("B", K * M * sizeof(double)));
    C = static_cast<double*>(dsm_alloc_named("C", N * M * sizeof(double)));  // zero-filled, like calloc
    if (!A || !B || !C) {
        fprintf(stderr, "[node %d] cannot allocate the matrices: %s (raise node_memory or max_nodes in dsm.conf)\n", me,
                dsm_strerror(dsm_last_error()));
        dsm_shutdown();
        return 2;
    }

    // Each node fills its own rows of A and of B (same partition as the work).
    long s, e;
    partition(N, me, numOfNodes, s, e);
    for (long i = s; i < e; i++)
        for (long j = 0; j < K; j++) A[i * K + j] = element(1, i, j);
    partition(K, me, numOfNodes, s, e);
    for (long i = s; i < e; i++)
        for (long j = 0; j < M; j++) B[i * M + j] = element(2, i, j);
    barrier(1);

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    matricesMultiplication(me);
    barrier(2);
    gettimeofday(&t1, NULL);
    double elapsed = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_usec - t0.tv_usec) / 1000000.0;
    printf("[node %d] Time for multiplication: %f sec\n", me, elapsed);

    int ok = 1;
    if (me == 0) {
        ok = verify(200);
        printf("[node 0] verification of 200 sampled entries: %s\n", ok ? "OK" : "FAIL");
        if (printResults != 0)
            for (long i = 0; i < N; i++) {
                for (long j = 0; j < M; j++) printf("%25.16f", C[i * M + j]);
                printf("\n");
            }
    }
    barrier(3);  // nobody leaves while node 0 still reads pages that live on other nodes
    printf("[node %d] %s\n", me, ok ? "RESULT: OK" : "RESULT: FAIL");
    dsm_shutdown();
    return ok ? 0 : 1;
}
