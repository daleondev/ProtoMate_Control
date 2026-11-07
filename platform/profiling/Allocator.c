#include "Trace.h"
#include <reent.h>
#include <stdint.h>
#include <stm32h7xx.h>
#include <tx_api.h>
#include <tx_thread.h>

/* Newlib's public allocation functions and C++ new converge on these entry
 * points. Depth is per thread; nested calloc/realloc calls count only once. */
static struct
{
    TX_THREAD* thread;
    unsigned depth;
} guards[32];
static unsigned boot_depth;
static unsigned* enter(void)
{
    unsigned saved = __get_PRIMASK();
    __disable_irq();
    unsigned* depth = &boot_depth;
    if (_tx_thread_current_ptr) {
        for (unsigned i = 0; i < 32; ++i) {
            if (guards[i].thread == _tx_thread_current_ptr && guards[i].depth) {
                depth = &guards[i].depth;
                goto found;
            }
        }
        for (unsigned i = 0; i < 32; ++i) {
            if (guards[i].depth == 0) {
                guards[i].thread = _tx_thread_current_ptr;
                depth = &guards[i].depth;
                break;
            }
        }
    }
found:
    ++*depth;
    __set_PRIMASK(saved);
    return depth;
}
static void leave(unsigned* depth, unsigned op, size_t size, void* address, uintptr_t caller, int failed)
{
    unsigned saved = __get_PRIMASK();
    __disable_irq();
    if (--*depth == 0)
        profile_allocation(op, (uint32_t)size, (uintptr_t)address, caller, failed);
    __set_PRIMASK(saved);
}
void* __real__malloc_r(struct _reent*, size_t);
void* __real__calloc_r(struct _reent*, size_t, size_t);
void* __real__realloc_r(struct _reent*, void*, size_t);
void* __real__memalign_r(struct _reent*, size_t, size_t);
void* __wrap__malloc_r(struct _reent* r, size_t size)
{
    unsigned* d = enter();
    void* p = __real__malloc_r(r, size);
    leave(d, 1, size, p, (uintptr_t)__builtin_return_address(0), !p && size != 0);
    return p;
}
void* __wrap__calloc_r(struct _reent* r, size_t n, size_t size)
{
    unsigned* d = enter();
    void* p = __real__calloc_r(r, n, size);
    size_t total = size && n > SIZE_MAX / size ? SIZE_MAX : n * size;
    leave(d, 2, total, p, (uintptr_t)__builtin_return_address(0), !p && total != 0);
    return p;
}
void* __wrap__realloc_r(struct _reent* r, void* old, size_t size)
{
    unsigned* d = enter();
    void* p = __real__realloc_r(r, old, size);
    leave(d, 3, size, p, (uintptr_t)__builtin_return_address(0), !p && size != 0);
    return p;
}
void* __wrap__memalign_r(struct _reent* r, size_t alignment, size_t size)
{
    unsigned* d = enter();
    void* p = __real__memalign_r(r, alignment, size);
    leave(d, 4, size, p, (uintptr_t)__builtin_return_address(0), !p && size != 0);
    return p;
}
