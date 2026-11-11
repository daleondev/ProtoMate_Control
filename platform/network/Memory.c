#include "Memory.h"
#include "ByteAccess.h"
#include "tx_api.h"
#include <stdint.h>

static _Alignas(32) __attribute__((section(".network_sdram.pool")))
    unsigned char storage[128 * 1024];
static TX_BYTE_POOL pool;
static NetworkMemoryStats statistics;
static int initialized;
typedef struct { void *allocation; size_t bytes; } Header;

int network_memory_initialize(void) {
    if(initialized) return 1;
    if(tx_byte_pool_create(&pool,"network.memory",storage,sizeof(storage))!=TX_SUCCESS) return 0;
    statistics=(NetworkMemoryStats){.capacity=sizeof(storage)};
    initialized=1;
    return 1;
}
void *network_allocate(size_t size) {
    if(!initialized || size>UINT32_MAX-32-sizeof(Header)) return NULL;
    void *raw=NULL;
    if(tx_byte_allocate(&pool,&raw,(ULONG)(size+32+sizeof(Header)),TX_NO_WAIT)!=TX_SUCCESS) {
        UINT previous=tx_interrupt_control(TX_INT_DISABLE); ++statistics.failures; tx_interrupt_control(previous);
        return NULL;
    }
    uintptr_t address=((uintptr_t)raw+sizeof(Header)+31U)&~(uintptr_t)31U;
    Header *h=(Header *)address-1;
    h->allocation=raw; h->bytes=size;
    UINT previous=tx_interrupt_control(TX_INT_DISABLE);
    statistics.used+=size; ++statistics.allocations;
    if(statistics.used>statistics.peak) statistics.peak=statistics.used;
    tx_interrupt_control(previous);
    return (void *)address;
}
void network_release(void *memory) {
    if(!memory) return;
    Header *h=(Header *)memory-1;
    void *raw=h->allocation; size_t size=h->bytes;
    UINT previous=tx_interrupt_control(TX_INT_DISABLE);
    statistics.used-=size;
    tx_interrupt_control(previous);
    tx_byte_release(raw);
}
void *network_callocate(size_t count, size_t size) {
    if(size && count>SIZE_MAX/size) return NULL;
    void *result=network_allocate(count*size);
    if(result) network_memset(result,0,count*size);
    return result;
}
void *network_resize(void *memory, size_t size) {
    if(!memory) return network_allocate(size);
    if(!size) { network_release(memory); return NULL; }
    Header *h=(Header *)memory-1;
    if(h->bytes>=size) return memory;
    void *next=network_allocate(size);
    if(next) { network_memcpy(next,memory,h->bytes); network_release(memory); }
    return next;
}
NetworkMemoryStats network_memory_stats(void) {
    UINT previous=tx_interrupt_control(TX_INT_DISABLE);
    NetworkMemoryStats result=statistics;
    tx_interrupt_control(previous);
    return result;
}
