// Public API of KADP-DSM. The only header an application includes.
//
// Prototype 1 (E2) subset: start/stop a node, find out who is in the cluster, and get memory
// from the shared address space. Locks, barriers and named allocations arrive in E3; until
// then applications synchronise through the shared memory itself (see apps/mm_dsm.cpp).
#pragma once
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

// Reads the INI file, reserves the shared address space, joins (or founds) the cluster and
// starts the node's threads. Returns DSM_OK (0) or a negative DSM_E* code (see dsm_strerror).
int dsm_init(const char* conf_path);

// Leaves cleanly and releases everything. Safe to call even if dsm_init failed.
int dsm_shutdown(void);

// This node's ID (0..max_nodes-1), or a negative error if dsm_init has not succeeded.
int dsm_node_id(void);

// Number of nodes this node currently knows to be ALIVE.
int dsm_num_nodes(void);

// Start of the memory range homed at `node` (same address on every node), or NULL.
// Writing to another node's range works: the page migrates here on first access.
void* dsm_home_base(int node);
size_t dsm_home_size(void);  // bytes per node
size_t dsm_space_size(void); // bytes of the whole shared space (max_nodes * node_memory)

// Page-aligned block taken from this node's own range. Prototype allocator: it only grows
// (no free) and cannot span several nodes; the real allocator comes with E3.
void* dsm_alloc(size_t size);

const char* dsm_strerror(int status);

#ifdef __cplusplus
}
#endif
