#ifndef PROTOMATE_OPCUA_MEMORY_H
#define PROTOMATE_OPCUA_MEMORY_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    size_t capacity;
    size_t used;
    size_t peak;
    size_t failures;
    size_t allocations;
    size_t failed_size;
    size_t largest_free_at_failure;
} OpcUaMemoryStats;
/* Single server-owner thread. Install before any open62541 allocation.
 * Storage must be max_align_t aligned and remain alive until all UA objects die. */
int opcua_memory_install(void *storage, size_t size);
/* Add a disjoint region before the first allocation, on the same owner thread. */
int opcua_memory_add_region(void *storage, size_t size);
OpcUaMemoryStats opcua_memory_stats(void);
void *opcua_memory_allocate(size_t size);
void opcua_memory_release(void *ptr);
void *opcua_memory_resize(void *ptr, size_t size);
void *opcua_memory_callocate(size_t count, size_t size);
#ifdef __cplusplus
}
#endif
#endif
