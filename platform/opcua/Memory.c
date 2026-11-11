#include "Memory.h"
#include <open62541/config.h>
#include <stdint.h>
#include <string.h>

typedef union Block Block;
union Block {
    struct { size_t size; Block *next; int available; } value;
    max_align_t alignment;
};

/* These are accessed exclusively by the server-owner thread. */
static Block *head;
static OpcUaMemoryStats stats;

int opcua_memory_install(void *storage, size_t size) {
    if(!storage || (uintptr_t)storage % _Alignof(max_align_t) ||
       size <= sizeof(Block) || (head && stats.used))
        return 0;
    size -= size % _Alignof(max_align_t);
    head = storage;
    *head = (Block){.value = {size - sizeof(Block), NULL, 1}};
    stats = (OpcUaMemoryStats){.capacity = size};
    UA_mallocSingleton = opcua_memory_allocate;
    UA_freeSingleton = opcua_memory_release;
    UA_reallocSingleton = opcua_memory_resize;
    UA_callocSingleton = opcua_memory_callocate;
    return 1;
}

int opcua_memory_add_region(void *storage, size_t size) {
    if(!head || stats.allocations || !storage ||
       (uintptr_t)storage % _Alignof(max_align_t) || size <= sizeof(Block) ||
       (uintptr_t)storage > UINTPTR_MAX - size || stats.capacity > SIZE_MAX - size)
        return 0;
    size -= size % _Alignof(max_align_t);
    Block *last = NULL;
    for(Block *b = head; b; b = b->value.next) {
        uintptr_t begin = (uintptr_t)b, end = begin + sizeof(Block) + b->value.size;
        if((uintptr_t)storage < end && (uintptr_t)storage + size > begin) return 0;
        last = b;
    }
    Block *added = storage;
    *added = (Block){.value = {size - sizeof(Block), NULL, 1}};
    last->value.next = added;
    stats.capacity += size;
    return 1;
}

void *opcua_memory_allocate(size_t size) {
    const size_t alignment = _Alignof(max_align_t);
    if(size == 0) size = 1;
    if(size > SIZE_MAX - (alignment - 1)) { ++stats.failures; return NULL; }
    size = (size + alignment - 1) / alignment * alignment;
    for(Block *b = head; b; b = b->value.next) {
        if(!b->value.available || b->value.size < size) continue;
        if(b->value.size - size >= sizeof(Block) + alignment) {
            Block *next = (Block *)((unsigned char *)(b + 1) + size);
            *next = (Block){.value = {b->value.size - size - sizeof(Block), b->value.next, 1}};
            b->value.size = size;
            b->value.next = next;
        }
        b->value.available = 0;
        stats.used += sizeof(Block) + b->value.size;
        if(stats.used > stats.peak) stats.peak = stats.used;
        ++stats.allocations;
        return b + 1;
    }
    ++stats.failures;
    return NULL;
}

void opcua_memory_release(void *ptr) {
    if(!ptr) return;
    Block *b = (Block *)ptr - 1;
    stats.used -= sizeof(Block) + b->value.size;
    b->value.available = 1;
    for(b = head; b && b->value.next;) {
        Block *next = b->value.next;
        if(b->value.available && next->value.available &&
           (uintptr_t)(b + 1) + b->value.size == (uintptr_t)next) {
            b->value.size += sizeof(Block) + next->value.size;
            b->value.next = next->value.next;
        } else b = next;
    }
}

void *opcua_memory_resize(void *ptr, size_t size) {
    if(!ptr) return opcua_memory_allocate(size);
    if(!size) { opcua_memory_release(ptr); return NULL; }
    Block *b = (Block *)ptr - 1;
    if(size <= b->value.size) return ptr;
    void *next = opcua_memory_allocate(size);
    if(!next) return NULL;
    memcpy(next, ptr, b->value.size);
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
