#if defined(HAL_PLATFORM_LINUX)

void runtime_libc_initialize(void)
{
    /* The ThreadX Linux port represents every simulated ThreadX thread with a
     * pthread. glibc therefore supplies both internal locking and per-pthread
     * libc state without Newlib's retargeting hooks. */
}

#elif defined(HAL_PLATFORM_STM32)

#include "cpu_clock.h"
#include "hal/hal.hpp"

#include <sys/lock.h>
#include <sys/reent.h>

#include <tx_api.h>
#include <tx_thread.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Newlib's global reentrancy pointer is normally installed by ThreadX, but
 * C++ constructors run before tx_kernel_enter(). Make pre-main allocation
 * use Newlib's global state instead of passing a null reentrancy pointer into
 * malloc. The lock hooks below already treat this phase as single-threaded. */
static void runtime_libc_preinitialize(void)
{
    _impure_ptr = &_impure_data;
}

__attribute__((used, section(".preinit_array")))
static void (*const runtime_libc_preinitializer)(void) = runtime_libc_preinitialize;

/* Newlib deliberately keeps this type opaque, allowing the target to store
 * its native lock directly in each static and dynamically allocated object. */
struct __lock
{
    TX_MUTEX mutex;
    struct __lock* next;
    bool created;
    bool allocated;
    bool dynamic;
};
typedef struct __lock __lock_t;

static CHAR arc4random_mutex_name[] = "Newlib arc4random";
static CHAR at_quick_exit_mutex_name[] = "Newlib quick exit";
static CHAR atexit_mutex_name[] = "Newlib atexit";
static CHAR dd_hash_mutex_name[] = "Newlib dtoa";
static CHAR env_mutex_name[] = "Newlib environment";
static CHAR malloc_mutex_name[] = "Newlib malloc";
static CHAR sfp_mutex_name[] = "Newlib stdio";
static CHAR tz_mutex_name[] = "Newlib timezone";
static CHAR dynamic_mutex_name[] = "Newlib dynamic";
static CHAR dynamic_pool_mutex_name[] = "Newlib lock pool";

static __lock_t* dynamic_locks;
static TX_MUTEX dynamic_pool_mutex;
static bool libc_locks_ready;

static _Noreturn void libc_lock_failure(void)
{
    Error_Handler();
    for (;;) {
    }
}

static bool locking_required(void)
{
    if (__get_IPSR() != 0U) {
        /* Newlib calls may block and are forbidden from interrupt context. */
        libc_lock_failure();
    }

    const TX_THREAD* current_thread = tx_thread_identify();

    if (current_thread == TX_NULL) {
        /* Before the scheduler starts, execution is single-threaded. */
        return false;
    }

    if (!libc_locks_ready) {
        libc_lock_failure();
    }

    return true;
}

static void acquire_libc_lock(_LOCK_T lock, ULONG wait_option)
{
    if (!locking_required()) {
        return;
    }

    if (lock == NULL || !lock->created || tx_mutex_get(&lock->mutex, wait_option) != TX_SUCCESS) {
        libc_lock_failure();
    }
}

static int try_acquire_libc_lock(_LOCK_T lock)
{
    if (!locking_required()) {
        return 1;
    }

    if (lock == NULL || !lock->created) {
        libc_lock_failure();
    }

    const UINT status = tx_mutex_get(&lock->mutex, TX_NO_WAIT);
    if (status == TX_SUCCESS) {
        return 1;
    }
    if (status == TX_NOT_AVAILABLE) {
        return 0;
    }

    libc_lock_failure();
}

static void release_libc_lock(_LOCK_T lock)
{
    if (!locking_required()) {
        return;
    }

    if (lock == NULL || !lock->created || tx_mutex_put(&lock->mutex) != TX_SUCCESS) {
        libc_lock_failure();
    }
}

/* These are the lock sentinels exported by Newlib's default lock object.
 * Providing them here keeps that object (and its no-op lock functions) out of
 * the link while preserving the ABI expected by the rest of Newlib. */
__lock_t __lock___arc4random_mutex;
__lock_t __lock___at_quick_exit_mutex;
__lock_t __lock___atexit_recursive_mutex;
__lock_t __lock___dd_hash_mutex;
__lock_t __lock___env_recursive_mutex;
__lock_t __lock___malloc_recursive_mutex;
__lock_t __lock___sfp_recursive_mutex;
__lock_t __lock___tz_mutex;

typedef struct
{
    __lock_t* lock;
    CHAR* name;
} static_lock_definition_t;

static int create_lock(__lock_t* lock, CHAR* name)
{
    if (tx_mutex_create(&lock->mutex, name, TX_INHERIT) != TX_SUCCESS) {
        return 1;
    }

    lock->created = true;
    return 0;
}

void runtime_libc_initialize(void)
{
    if (libc_locks_ready) {
        return;
    }

    static_lock_definition_t static_locks[] = {
        { &__lock___arc4random_mutex, arc4random_mutex_name },
        { &__lock___at_quick_exit_mutex, at_quick_exit_mutex_name },
        { &__lock___atexit_recursive_mutex, atexit_mutex_name },
        { &__lock___dd_hash_mutex, dd_hash_mutex_name },
        { &__lock___env_recursive_mutex, env_mutex_name },
        { &__lock___malloc_recursive_mutex, malloc_mutex_name },
        { &__lock___sfp_recursive_mutex, sfp_mutex_name },
        { &__lock___tz_mutex, tz_mutex_name },
    };

    for (size_t index = 0; index < sizeof(static_locks) / sizeof(static_locks[0]); ++index) {
        if (create_lock(static_locks[index].lock, static_locks[index].name) != 0) {
            libc_lock_failure();
        }
    }

    for (__lock_t* lock = dynamic_locks; lock != NULL; lock = lock->next) {
        if (create_lock(lock, dynamic_mutex_name) != 0) {
            libc_lock_failure();
        }
    }

    if (tx_mutex_create(&dynamic_pool_mutex, dynamic_pool_mutex_name, TX_INHERIT) != TX_SUCCESS) {
        libc_lock_failure();
    }

    libc_locks_ready = true;
}

static void lock_dynamic_pool(void)
{
    if (locking_required() && tx_mutex_get(&dynamic_pool_mutex, TX_WAIT_FOREVER) != TX_SUCCESS) {
        libc_lock_failure();
    }
}

static void unlock_dynamic_pool(void)
{
    if (locking_required() && tx_mutex_put(&dynamic_pool_mutex) != TX_SUCCESS) {
        libc_lock_failure();
    }
}

void __retarget_lock_init(_LOCK_T* lock)
{
    if (lock == NULL) {
        libc_lock_failure();
    }

    lock_dynamic_pool();

    for (__lock_t* candidate = dynamic_locks; candidate != NULL; candidate = candidate->next) {
        if (!candidate->allocated) {
            candidate->allocated = true;
            *lock = candidate;
            unlock_dynamic_pool();
            return;
        }
    }

    __lock_t* const created_lock = (__lock_t*)calloc(1U, sizeof(*created_lock));
    if (created_lock != NULL) {
        created_lock->allocated = true;
        created_lock->dynamic = true;
        created_lock->next = dynamic_locks;
        dynamic_locks = created_lock;

        /* Before tx_kernel_enter() there is no scheduler and lock acquisition
         * is a no-op. runtime_libc_initialize() creates every such deferred
         * mutex before the first ThreadX thread can run. */
        if (!libc_locks_ready || create_lock(created_lock, dynamic_mutex_name) == 0) {
            *lock = created_lock;
            unlock_dynamic_pool();
            return;
        }
    }

    unlock_dynamic_pool();
    libc_lock_failure();
}

void __retarget_lock_init_recursive(_LOCK_T* lock) { __retarget_lock_init(lock); }

void __retarget_lock_close(_LOCK_T lock)
{
    if (lock == NULL) {
        libc_lock_failure();
    }

    lock_dynamic_pool();
    for (__lock_t* candidate = dynamic_locks; candidate != NULL; candidate = candidate->next) {
        if (lock == candidate && candidate->dynamic) {
            candidate->allocated = false;
            unlock_dynamic_pool();
            return;
        }
    }
    unlock_dynamic_pool();
}

void __retarget_lock_close_recursive(_LOCK_T lock) { __retarget_lock_close(lock); }

void __retarget_lock_acquire(_LOCK_T lock) { acquire_libc_lock(lock, TX_WAIT_FOREVER); }

void __retarget_lock_acquire_recursive(_LOCK_T lock) { __retarget_lock_acquire(lock); }

int __retarget_lock_try_acquire(_LOCK_T lock) { return try_acquire_libc_lock(lock); }

int __retarget_lock_try_acquire_recursive(_LOCK_T lock) { return __retarget_lock_try_acquire(lock); }

void __retarget_lock_release(_LOCK_T lock) { release_libc_lock(lock); }

void __retarget_lock_release_recursive(_LOCK_T lock) { __retarget_lock_release(lock); }

void runtime_libc_thread_create(TX_THREAD* thread_ptr)
{
    if (thread_ptr == TX_NULL) {
        libc_lock_failure();
    }

    _REENT_INIT_PTR(&thread_ptr->tx_thread_libc_reent);
}

void runtime_libc_thread_delete(TX_THREAD* thread_ptr)
{
    if (thread_ptr == TX_NULL) {
        libc_lock_failure();
    }

    if (_impure_ptr == &thread_ptr->tx_thread_libc_reent) {
        _impure_ptr = &_impure_data;
    }
    _reclaim_reent(&thread_ptr->tx_thread_libc_reent);
}

void _tx_execution_initialize(void)
{
    _impure_ptr = &_impure_data;
    runtime_cpu_clock_initialize();
}

void _tx_execution_thread_enter(void)
{
    if (_tx_thread_current_ptr == TX_NULL) {
        _impure_ptr = &_impure_data;
    }
    else {
        _impure_ptr = &_tx_thread_current_ptr->tx_thread_libc_reent;
        runtime_cpu_clock_thread_enter();
    }
}

void _tx_execution_thread_exit(void)
{
    runtime_cpu_clock_thread_exit();
    _impure_ptr = &_impure_data;
}

/* The 1 kHz SysTick samples even when a thread runs uninterrupted or the
 * scheduler stays idle, extending DWT's 32-bit cycle counter across wraps. */
void _tx_execution_isr_enter(void) { runtime_cpu_clock_sample(); }

void _tx_execution_isr_exit(void) { runtime_cpu_clock_sample(); }

#endif
