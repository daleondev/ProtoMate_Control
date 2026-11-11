#include "Memory.h"
#include <open62541/config.h>
#include <stdint.h>
#include <string.h>

/* Boundary tags: the low size bit marks a free block; the preceding payload
 * size finds its physical neighbour. No next/previous pointers are stored in
 * live blocks. This saves 8 bytes per allocation on M7 (thousands of nodes and
 * metadata strings), without reducing alignment or changing the arena budget. */
typedef struct Block {
    _Alignas(max_align_t) size_t size_flags;
    size_t previous_size;
} Block;
_Static_assert(sizeof(Block) % _Alignof(max_align_t) == 0, "aligned block payload");
typedef struct { Block *next, *previous; } FreeLinks;
typedef struct { uintptr_t begin, end; } Region;
static Region regions[4];
static unsigned region_count;
static Block *free_head;
static OpcUaMemoryStats stats;
static size_t block_size(const Block *b) { return b->size_flags & ~(size_t)1; }
static int available(const Block *b) { return (b->size_flags & 1) != 0; }
static FreeLinks *links(Block *b) { return (FreeLinks *)(b + 1); }
static Block *next_block(Block *b) {
    uintptr_t address = (uintptr_t)b;
    uintptr_t next = (uintptr_t)(b + 1) + block_size(b);
    for(unsigned i=0; i<region_count; ++i)
        if(address >= regions[i].begin && address < regions[i].end)
            return next < regions[i].end ? (Block *)next : NULL;
    return NULL;
}
static Block *previous_block(Block *b) {
    return b->previous_size ? (Block *)((unsigned char *)b - sizeof(Block) - b->previous_size) : NULL;
}
static void insert_free(Block *b) {
    *links(b) = (FreeLinks){free_head, NULL};
    if(free_head) links(free_head)->previous = b;
    free_head = b;
}
static void remove_free(Block *b) {
    FreeLinks value = *links(b);
    if(value.previous) links(value.previous)->next = value.next; else free_head = value.next;
    if(value.next) links(value.next)->previous = value.previous;
}
static void merge_next(Block *b, Block *next) {
    b->size_flags = (block_size(b) + sizeof(Block) + block_size(next)) | 1;
    Block *following = next_block(b);
    if(following) following->previous_size = block_size(b);
}
static int valid_region(void *storage, size_t size) {
    return storage && (uintptr_t)storage % _Alignof(max_align_t) == 0 &&
        size >= sizeof(Block) + sizeof(FreeLinks) && (uintptr_t)storage <= UINTPTR_MAX - size;
}
static void add_region(void *storage, size_t size) {
    regions[region_count++] = (Region){(uintptr_t)storage, (uintptr_t)storage + size};
    Block *first = storage;
    *first = (Block){(size - sizeof(Block)) | 1, 0};
    insert_free(first);
    stats.capacity += size;
}
int opcua_memory_install(void *storage, size_t size) {
    if(!valid_region(storage, size) || (region_count && stats.used)) return 0;
    size -= size % _Alignof(max_align_t);
    region_count = 0; free_head = NULL;
    stats = (OpcUaMemoryStats){0};
    add_region(storage, size);
    UA_mallocSingleton = opcua_memory_allocate;
    UA_freeSingleton = opcua_memory_release;
    UA_reallocSingleton = opcua_memory_resize;
    UA_callocSingleton = opcua_memory_callocate;
    return 1;
}
int opcua_memory_add_region(void *storage, size_t size) {
    if(!region_count || region_count == 4 || stats.allocations ||
       !valid_region(storage, size) || stats.capacity > SIZE_MAX - size) return 0;
    size -= size % _Alignof(max_align_t);
    for(unsigned i=0; i<region_count; ++i)
        if((uintptr_t)storage < regions[i].end && (uintptr_t)storage + size > regions[i].begin) return 0;
    add_region(storage, size);
    return 1;
}
void *opcua_memory_allocate(size_t size) {
    const size_t alignment = _Alignof(max_align_t);
    if(size < sizeof(FreeLinks)) size = sizeof(FreeLinks);
    if(size > SIZE_MAX - (alignment - 1)) { ++stats.failures; return NULL; }
    size = (size + alignment - 1) / alignment * alignment;
    size_t largest = 0;
    Block *best = NULL;
    /* Best-fit preserves the fixed 8 KiB network chunks amid tiny metadata. */
    for(Block *b = free_head; b; b = links(b)->next) {
        const size_t extent = block_size(b);
        if(extent > largest) largest = extent;
        if(extent >= size && (!best || extent < block_size(best))) best = b;
        if(best && block_size(best) == size) break;
    }
    if(best) {
        Block *b = best;
        remove_free(b);
        const size_t extent = block_size(b);
        if(extent - size >= sizeof(Block) + sizeof(FreeLinks)) {
            Block *next = (Block *)((unsigned char *)(b + 1) + size);
            *next = (Block){(extent - size - sizeof(Block)) | 1, size};
            b->size_flags = size;
            Block *following = next_block(next);
            if(following) following->previous_size = block_size(next);
            insert_free(next);
        } else b->size_flags = extent;
        stats.used += sizeof(Block) + block_size(b);
        if(stats.used > stats.peak) stats.peak = stats.used;
        ++stats.allocations;
        return b + 1;
    }
    ++stats.failures; stats.failed_size = size; stats.largest_free_at_failure = largest;
    return NULL;
}
void opcua_memory_release(void *ptr) {
    if(!ptr) return;
    Block *b = (Block *)ptr - 1;
    stats.used -= sizeof(Block) + block_size(b);
    b->size_flags |= 1;
    Block *next = next_block(b);
    if(next && available(next)) { remove_free(next); merge_next(b, next); }
    Block *previous = previous_block(b);
    if(previous && available(previous)) {
        remove_free(previous); merge_next(previous, b); b = previous;
    }
    insert_free(b);
}
void *opcua_memory_resize(void *ptr, size_t size) {
    if(!ptr) return opcua_memory_allocate(size);
    if(!size) { opcua_memory_release(ptr); return NULL; }
    Block *b = (Block *)ptr - 1;
    if(size <= block_size(b)) return ptr;
    void *next = opcua_memory_allocate(size);
    if(!next) return NULL;
    memcpy(next, ptr, block_size(b));
    opcua_memory_release(ptr);
    return next;
}
void *opcua_memory_callocate(size_t count, size_t size) {
    if(size && count > SIZE_MAX / size) { ++stats.failures; return NULL; }
    size *= count;
    void *p = opcua_memory_allocate(size);
    if(p) memset(p, 0, size);
    return p;
}
OpcUaMemoryStats opcua_memory_stats(void) { return stats; }
