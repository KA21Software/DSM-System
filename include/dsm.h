// Public API of KADP-DSM. The only header an application includes.
//
// Names follow the E2 prototype (dsm_shutdown, dsm_num_nodes, int dsm_node_id); E3 only adds
// functions. Every function that returns int gives DSM_OK (0) or a negative DSM_E* code
// (dsm_strerror). Functions that return a pointer give NULL on failure and leave the code in
// dsm_last_error().
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Error codes (design table 6.6), shared with the wire protocol.
enum {
    DSM_OK = 0, DSM_EINVAL = -1, DSM_ENOMEM = -2, DSM_EEXIST = -3, DSM_ENOTFOUND = -4,
    DSM_ENODEDOWN = -5, DSM_ETIMEOUT = -6, DSM_ENET = -7, DSM_ECONFIG = -8, DSM_ENOTINIT = -9,
    DSM_EFULL = -10, DSM_EPROTO = -11, DSM_ENOTALLOC = -12
};

typedef struct {
    uint16_t node_id, n_nodes_alive;
    uint32_t pages_home_total, pages_home_used;
    uint32_t pages_cached_s, pages_cached_m;
    uint64_t faults_read, faults_write;
    uint64_t pages_sent, pages_received;
    uint64_t invalidations_sent, invalidations_received;
    uint64_t bytes_sent, bytes_received;
    uint32_t avg_fault_us, max_fault_us;  // time to resolve a page fault
} dsm_stats_t;                            // 96 bytes

// Reads the INI file, reserves the shared address space, joins (or founds) the cluster and
// starts the node's threads. If stdin is a terminal (or DSM_CONSOLE=1) the console of
// design section 10 runs too.
int dsm_init(const char* conf_path);

// Tells the cluster this node is leaving and releases everything. Call it after a final
// barrier: pages homed here disappear with the node. Safe to call even if dsm_init failed.
int dsm_shutdown(void);

int dsm_node_id(void);     // 0..max_nodes-1, or a negative error before dsm_init
int dsm_num_nodes(void);  // nodes this node currently sees as ALIVE or SUSPECT

// Start of the memory range homed at `node` (same address on every node), or NULL.
void* dsm_home_base(int node);
size_t dsm_home_size(void);   // bytes per node
size_t dsm_space_size(void);  // bytes of the whole shared space (max_nodes * node_memory)

// Page-aligned, zero-filled block of contiguous pages; the pointer is valid on every node.
// It may cross the ranges of several nodes. Errors: DSM_EINVAL (size 0), DSM_ENOMEM, DSM_ENODEDOWN.
void* dsm_alloc(size_t size);
// Creates the block and its name (<= 31 chars), or returns the existing block of that name if
// it has the same size (DSM_EEXIST if the size differs). Every node may call it.
void* dsm_alloc_named(const char* name, size_t size);
void* dsm_lookup(const char* name, size_t* size_out);  // DSM_ENOTFOUND if there is none
int dsm_free(void* ptr);                               // ptr must be what dsm_alloc returned

// Distributed mutual exclusion and rendezvous, run by the coordinator.
int dsm_lock(uint32_t lock_id);
int dsm_unlock(uint32_t lock_id);
int dsm_barrier(uint32_t barrier_id, uint16_t n_participants);

int dsm_get_stats(dsm_stats_t* out);
int dsm_get_node_stats(uint16_t node_id, dsm_stats_t* out);
int dsm_get_global_stats(dsm_stats_t* out);  // sum over the live nodes

int dsm_last_error(void);  // of the calling thread
const char* dsm_strerror(int status);

#ifdef __cplusplus
}
#endif
