#pragma once

/* No RTOS, HAL or application types in the instrumentation interface. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#define PROFILE_NOEXCEPT noexcept
#else
#define PROFILE_NOEXCEPT
#endif

enum ProfilePreset
{
    PROFILE_SYSTEM = 0,
    PROFILE_MOTION,
    PROFILE_GRAPHICS,
    PROFILE_MEMORY
};
enum ProfileState
{
    PROFILE_UNAVAILABLE = 0,
    PROFILE_READY,
    PROFILE_RECORDING,
    PROFILE_FROZEN,
    PROFILE_PREPARING
};
enum ProfileReason
{
    PROFILE_MANUAL = 0,
    PROFILE_ALLOCATION,
    PROFILE_CONNECTION,
    PROFILE_MOTION_GUARD,
    PROFILE_STACK,
    PROFILE_FATAL
};
enum ProfileCategory
{
    PROFILE_CAT_SYSTEM = 1,
    PROFILE_CAT_MOTION = 2,
    PROFILE_CAT_GRAPHICS = 4,
    PROFILE_CAT_MEMORY = 8,
    PROFILE_CAT_IRQ = 16
};
/* TraceX user IDs are 4096..65535. Keep this catalog stable across releases. */
enum ProfileEvent
{
    PROFILE_CAPTURE = 4096,
    PROFILE_ANCHOR,
    PROFILE_FAILURE,
    PROFILE_ALLOCATION_EVENT,
    PROFILE_DURATION,
    PROFILE_QUEUE,
    PROFILE_HEAP,
    PROFILE_STACK_EVENT,
    PROFILE_SCHEDULE,
    PROFILE_ICC_STAGE = 4120,
    PROFILE_ICC_STATE,
    PROFILE_ICC_FRAME,
    PROFILE_MOTION_STAGE = 4140,
    PROFILE_MOTION_JOB,
    PROFILE_GRAPHICS_STAGE = 4160
};
enum ProfileSite
{
    PROFILE_SITE_ICC_SEND = 0,
    PROFILE_SITE_ICC_DISPATCH,
    PROFILE_SITE_MOTION_PREPARE,
    PROFILE_SITE_MOTION_RENEW,
    PROFILE_SITE_RENDER,
    PROFILE_SITE_PRESENT,
    PROFILE_SITE_PLANNER
};
enum
{
    PROFILE_SITE_COUNT = 64,
    PROFILE_QUEUE_COUNT = 32,
    PROFILE_THREAD_COUNT = 32,
    PROFILE_TRACE_BYTES = 128 * 1024,
    PROFILE_SUMMARY_BYTES = 32 * 1024
};

typedef struct
{
    uint32_t sequence, time_us, operation, size, caller, thread, failure_count, reason;
} ProfileFailure;
typedef struct
{
    uint32_t state, preset, reason, capture, core, frequency_hz;
    uint32_t started_us, frozen_us, wraps, events, filtered, registry_free;
    uint32_t allocation_calls, allocation_failures, last_requested, sampled_min_free;
    uint32_t heap_available, heap_used, snapshots, clock_valid;
    ProfileFailure failure;
} ProfileStatus;
typedef struct
{
    uint32_t count, minimum, maximum, invalid;
    uint64_t total;
    uint32_t histogram[16]; /* power-of-two microsecond buckets; last is overflow */
} ProfileMeasurement;
typedef struct
{
    uint32_t current, high_water, capacity, blocked, dropped, replaced;
} ProfileQueue;
typedef struct
{
    uint32_t id, priority, stack_size, peak_stack;
    uint64_t cycles;
    char name[32];
} ProfileThread;
typedef struct
{
    ProfileMeasurement sites[PROFILE_SITE_COUNT];
    ProfileQueue queues[PROFILE_QUEUE_COUNT];
    ProfileThread threads[PROFILE_THREAD_COUNT];
    uint64_t idle_cycles, interrupt_cycles, unclassified_cycles;
    uint32_t thread_overflow, irq_depth, accounting_valid, anchor_epoch;
} ProfileSummary;

/* Debugger mailbox: arguments first, request sequence last; worker acknowledges
 * only after completion. Never write recorder internals through the debugger. */
typedef struct
{
    uint32_t magic, version, status_size, summary_size;
    volatile uint32_t request, operation, preset, capture, acknowledge, result;
    uint32_t trace_bytes, registry_entries, word_bytes;
    ProfileStatus status;
} ProfileControl;
extern ProfileControl protomate_profile;
extern ProfileSummary protomate_profile_summary;

void profile_initialize(void) PROFILE_NOEXCEPT;
void profile_allow_arm(int allowed) PROFILE_NOEXCEPT;
void profile_service(void) PROFILE_NOEXCEPT;
int profile_arm(uint32_t preset, uint32_t capture) PROFILE_NOEXCEPT;
void profile_freeze(uint32_t reason) PROFILE_NOEXCEPT;
ProfileStatus profile_status(void) PROFILE_NOEXCEPT;
void profile_failure(uint32_t reason, uint32_t operation, uint32_t size, uintptr_t caller) PROFILE_NOEXCEPT;
void profile_heap(uint32_t available, uint32_t used) PROFILE_NOEXCEPT;
void profile_stack(uintptr_t thread, const char* name, uint32_t priority, uint32_t size, uint32_t peak)
  PROFILE_NOEXCEPT;
void profile_allocation(uint32_t operation, uint32_t size, uintptr_t address, uintptr_t caller, int failed)
  PROFILE_NOEXCEPT;
void profile_thread_switch(uintptr_t thread, uint32_t priority) PROFILE_NOEXCEPT;
void profile_irq_enter(uint32_t irq) PROFILE_NOEXCEPT;
void profile_irq_exit(uint32_t irq) PROFILE_NOEXCEPT;
uint32_t profile_cycles(void) PROFILE_NOEXCEPT;
uint32_t profile_time_us(void) PROFILE_NOEXCEPT;

#if defined(PROTOMATE_PROFILING)
uint32_t profile_generation(void) PROFILE_NOEXCEPT;
void profile_event(uint32_t category, uint32_t event, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
  PROFILE_NOEXCEPT;
void profile_measure(uint32_t site,
                     uint32_t cycles,
                     uint32_t elapsed_us,
                     uint32_t category,
                     uint32_t generation) PROFILE_NOEXCEPT;
void profile_queue(uint32_t slot,
                   uint32_t current,
                   uint32_t capacity,
                   uint32_t blocked,
                   uint32_t dropped,
                   uint32_t replaced) PROFILE_NOEXCEPT;
/* Subscription totals are lifetime counters, sampled into slots 6 and 16..31. */
void profile_subscription(uint32_t slot, uint32_t current, uint32_t capacity, uint32_t dropped)
  PROFILE_NOEXCEPT;
/* Called with the same interrupt protection as ThreadX's native insertion. */
void profile_tx_insert(uint32_t event, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uint32_t filter)
  PROFILE_NOEXCEPT;
#else
#define profile_event(...) ((void)0)
#define profile_measure(...) ((void)0)
#define profile_queue(...) ((void)0)
#define profile_subscription(...) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#undef PROFILE_NOEXCEPT
