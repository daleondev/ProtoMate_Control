#pragma once
#include <stddef.h>
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { size_t capacity, used, peak, failures, allocations; } NetworkMemoryStats;
int network_memory_initialize(void);
void *network_allocate(size_t size);
void network_release(void *memory);
void *network_callocate(size_t count, size_t size);
void *network_resize(void *memory, size_t size);
NetworkMemoryStats network_memory_stats(void);
#ifdef __cplusplus
}
#endif
#ifdef NETWORK_ALLOCATOR_OVERRIDE
#define malloc network_allocate
#define calloc network_callocate
#define realloc network_resize
#define free network_release
#endif
