#include "Trace.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <tx_api.h>
#include <tx_thread.h>
#if defined(__arm__)
#include <stm32h7xx.h>
#else
#include <time.h>
#endif
#if defined(PROTOMATE_PROFILING)
#include <tx_trace.h>
#endif

#if defined(CORE_CM7)
/* SWD is not coherent with M7's D-cache. Both the debugger mailbox and the
 * frozen first-failure record must be visible without cache maintenance. */
__attribute__((section(".dtcm.profile_control"), aligned(8)))
#endif
ProfileControl protomate_profile = { .magic = 0x50524631U,
                                     .version = 1,
                                     .status_size = sizeof(ProfileStatus),
                                     .summary_size = sizeof(ProfileSummary),
                                     .trace_bytes = PROFILE_TRACE_BYTES,
                                     .registry_entries = 256,
                                     .word_bytes = sizeof(ULONG) };
static int arm_allowed = 1;
static int initialized;
void profile_allow_arm(int allowed) { __atomic_store_n(&arm_allowed, allowed, __ATOMIC_RELEASE); }

#if defined(PROTOMATE_PROFILING)
#if defined(__arm__)
#define TRACE_STORAGE __attribute__((section(".profile_trace"), aligned(32)))
#define SUMMARY_STORAGE __attribute__((section(".profile_summary"), aligned(32)))
#else
#define TRACE_STORAGE __attribute__((aligned(32)))
#define SUMMARY_STORAGE __attribute__((aligned(32)))
#endif
TRACE_STORAGE unsigned char protomate_profile_trace[PROFILE_TRACE_BYTES];
SUMMARY_STORAGE ProfileSummary protomate_profile_summary;
_Static_assert(sizeof(ProfileSummary) <= PROFILE_SUMMARY_BYTES, "Profiling summary budget");
static uint32_t last_cycles, current_thread, last_anchor, last_time;
static uint32_t irq_depth, generation;
typedef struct
{
    uint32_t id, priority;
    uint64_t cycles;
} CpuThread;
static CpuThread cpu_threads[PROFILE_THREAD_COUNT];
static CpuThread* active_cpu;
static uint64_t idle_cycles, interrupt_cycles, unclassified_cycles;
static uint32_t thread_overflow;
#endif

static unsigned lock(void)
{
#if defined(__arm__)
    unsigned saved = __get_PRIMASK();
    __disable_irq();
    return saved;
#else
    if (!initialized) return UINT_MAX; // Before the hosted ThreadX mutex exists.
    return tx_interrupt_control(TX_INT_DISABLE);
#endif
}
static void unlock(unsigned saved)
{
#if defined(__arm__)
    __set_PRIMASK(saved);
#else
    if (saved != UINT_MAX) (void)tx_interrupt_control(saved);
#endif
}
uint32_t profile_time_us(void)
{
#if defined(__arm__)
    return TIM5->CNT;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000U + (uint64_t)ts.tv_nsec / 1000U);
#endif
}
uint32_t profile_cycles(void)
{
#if defined(__arm__)
    return DWT->CYCCNT;
#else
    return profile_time_us();
#endif
}
static uint32_t thread_id(void) { return (uint32_t)(uintptr_t)_tx_thread_current_ptr; }

#if defined(PROTOMATE_PROFILING)
static void wrapped(void* unused)
{
    (void)unused;
    ++protomate_profile.status.wraps;
}
static int enabled(uint32_t category)
{
    const uint32_t p = protomate_profile.status.preset;
    return (category & PROFILE_CAT_SYSTEM) ||
           (p == PROFILE_MOTION && (category & (PROFILE_CAT_MOTION | PROFILE_CAT_IRQ))) ||
           (p == PROFILE_GRAPHICS && (category & (PROFILE_CAT_GRAPHICS | PROFILE_CAT_IRQ))) ||
           (p == PROFILE_MEMORY && (category & PROFILE_CAT_MEMORY));
}
static CpuThread* cpu_slot(uint32_t id)
{
    if (!id)
        return NULL;
    for (unsigned i = 0; i < PROFILE_THREAD_COUNT; ++i) {
        CpuThread* slot = &cpu_threads[i];
        if (slot->id == id)
            return slot;
        if (!slot->id) {
            slot->id = id;
            return slot;
        }
    }
    ++thread_overflow;
    return NULL;
}
static void flush_accounting(void)
{
    for (unsigned i = 0; i < PROFILE_THREAD_COUNT; ++i) {
        ProfileThread* to = &protomate_profile_summary.threads[i];
        to->id = cpu_threads[i].id;
        to->priority = cpu_threads[i].priority;
        to->cycles = cpu_threads[i].cycles;
    }
    protomate_profile_summary.idle_cycles = idle_cycles;
    protomate_profile_summary.interrupt_cycles = interrupt_cycles;
    protomate_profile_summary.unclassified_cycles = unclassified_cycles;
    protomate_profile_summary.thread_overflow = thread_overflow;
}
static void account(void)
{
    const uint32_t now = profile_cycles();
    const uint32_t delta = now - last_cycles;
    last_cycles = now;
    if (protomate_profile.status.state != PROFILE_RECORDING)
        return;
    if (irq_depth)
        interrupt_cycles += delta;
    else if (!current_thread)
        idle_cycles += delta;
    else if (active_cpu)
        active_cpu->cycles += delta;
    else
        unclassified_cycles += delta;
}
uint32_t profile_generation(void)
{
    const unsigned saved = lock();
    uint32_t value = protomate_profile.status.state == PROFILE_RECORDING ? generation : 0;
    unlock(saved);
    return value;
}
void profile_event(uint32_t category, uint32_t event, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    if (protomate_profile.status.state != PROFILE_RECORDING)
        return;
    const unsigned saved = lock();
    if (protomate_profile.status.state != PROFILE_RECORDING) {
        unlock(saved);
        return;
    }
    if (enabled(category) && event >= 4096 && event <= 65535)
        profile_tx_insert(event, a, b, c, d, TX_TRACE_USER_EVENTS);
    else
        ++protomate_profile.status.filtered;
    unlock(saved);
}
void profile_measure(uint32_t site,
                     uint32_t duration,
                     uint32_t elapsed_us,
                     uint32_t category,
                     uint32_t expected_generation)
{
    if (site >= PROFILE_SITE_COUNT || protomate_profile.status.state != PROFILE_RECORDING)
        return;
    const unsigned saved = lock();
    if (protomate_profile.status.state != PROFILE_RECORDING ||
        (expected_generation && expected_generation != generation)) {
        unlock(saved);
        return;
    }
    ProfileMeasurement* m = &protomate_profile_summary.sites[site];
    /* Counter ambiguity is explicit; no silently wrapped long measurements. */
    const uint64_t expected = (uint64_t)elapsed_us * protomate_profile.status.frequency_hz / 1000000U;
    if (expected >= UINT32_MAX) {
        ++m->invalid;
        unlock(saved);
        return;
    }
    if (!m->count || duration < m->minimum)
        m->minimum = duration;
    if (duration > m->maximum)
        m->maximum = duration;
    ++m->count;
    m->total += duration;
    uint32_t us = elapsed_us;
    unsigned bucket = 0;
    while (us > 1 && bucket < 15) {
        us >>= 1U;
        ++bucket;
    }
    ++m->histogram[bucket];
    profile_event(category, PROFILE_DURATION, site, duration, elapsed_us, thread_id());
    unlock(saved);
}
void profile_queue(uint32_t slot,
                   uint32_t current,
                   uint32_t capacity,
                   uint32_t blocked,
                   uint32_t dropped,
                   uint32_t replaced)
{
    if (slot >= PROFILE_QUEUE_COUNT || protomate_profile.status.state != PROFILE_RECORDING)
        return;
    const unsigned saved = lock();
    if (protomate_profile.status.state != PROFILE_RECORDING) {
        unlock(saved);
        return;
    }
    ProfileQueue* q = &protomate_profile_summary.queues[slot];
    q->current = current;
    q->capacity = capacity;
    if (current > q->high_water)
        q->high_water = current;
    q->blocked += blocked;
    q->dropped += dropped;
    q->replaced += replaced;
    if (blocked || dropped || replaced)
        profile_event(PROFILE_CAT_SYSTEM, PROFILE_QUEUE, slot, current, blocked, dropped + replaced);
    unlock(saved);
}
void profile_subscription(uint32_t slot, uint32_t current, uint32_t capacity, uint32_t dropped)
{
    if (slot >= PROFILE_QUEUE_COUNT)
        return;
    const unsigned saved = lock();
    if (protomate_profile.status.state == PROFILE_RECORDING) {
        ProfileQueue* q = &protomate_profile_summary.queues[slot];
        q->current = current;
        q->capacity = capacity;
        if (current > q->high_water)
            q->high_water = current;
        if (capacity == 1)
            q->replaced = dropped;
        else
            q->dropped = dropped;
    }
    unlock(saved);
}
#endif

void profile_initialize(void)
{
    if (initialized)
        return;
    initialized = 1;
#if defined(PROTOMATE_PROFILING)
    memset(&protomate_profile_summary, 0, sizeof(protomate_profile_summary));
    protomate_profile.status.state = PROFILE_READY;
#endif
#if defined(CORE_CM4)
    protomate_profile.status.core = 4;
#elif defined(CORE_CM7)
    protomate_profile.status.core = 7;
#endif
#if defined(__arm__)
    protomate_profile.status.frequency_hz = SystemCoreClock;
#else
    protomate_profile.status.frequency_hz = 1000000U; /* hosted elapsed time, not CPU cycles */
#endif
    protomate_profile.status.sampled_min_free = UINT32_MAX;
}
int profile_arm(uint32_t preset, uint32_t capture)
{
#if defined(PROTOMATE_PROFILING)
    if (!initialized || preset > PROFILE_MEMORY)
        return 1;
    if (!__atomic_load_n(&arm_allowed, __ATOMIC_ACQUIRE))
        return 5;
#if defined(__arm__)
    if (!((RCC->APB1LENR | RCC_C2->APB1LENR) & RCC_APB1LENR_TIM5EN) || !(TIM5->CR1 & TIM_CR1_CEN) ||
        TIM5->PSC != 239 || TIM5->ARR != UINT32_MAX)
        return 2;
#endif
    unsigned saved = lock();
    if (protomate_profile.status.state == PROFILE_RECORDING ||
        protomate_profile.status.state == PROFILE_PREPARING) {
        unlock(saved);
        return 3;
    }
    protomate_profile.status.state = PROFILE_PREPARING;
    unlock(saved);
    memset(&protomate_profile_summary, 0, sizeof(protomate_profile_summary));
    memset(cpu_threads, 0, sizeof(cpu_threads));
    idle_cycles = interrupt_cycles = unclassified_cycles = 0;
    thread_overflow = 0;
    saved = lock();
    if (!__atomic_load_n(&arm_allowed, __ATOMIC_ACQUIRE)) {
        protomate_profile.status.state = PROFILE_READY;
        unlock(saved);
        return 5;
    }
    ProfileStatus* s = &protomate_profile.status;
    s->preset = preset;
    s->capture = capture;
    s->reason = PROFILE_MANUAL;
    s->started_us = profile_time_us();
    s->frozen_us = 0;
    s->wraps = s->events = s->filtered = 0;
    s->clock_valid = 1;
    memset(&s->failure, 0, sizeof(s->failure));
    last_cycles = profile_cycles();
    last_time = last_anchor = s->started_us;
    current_thread = thread_id();
    active_cpu = cpu_slot(current_thread);
    irq_depth = 0;
#if defined(__arm__)
    protomate_profile_summary.accounting_valid = 1;
#endif
    const UINT result = tx_trace_enable(
      protomate_profile_trace, sizeof(protomate_profile_trace), protomate_profile.registry_entries);
    if (result != TX_SUCCESS) {
        s->state = PROFILE_READY;
        unlock(saved);
        return (int)result;
    }
    (void)tx_trace_buffer_full_notify(wrapped);
    /* Tick/time-slice traffic obscures useful scheduling and synchronization. */
    (void)tx_trace_event_filter(TX_TRACE_TIMER_EVENTS | TX_TRACE_TIME_EVENTS);
    if (preset == PROFILE_MOTION || preset == PROFILE_GRAPHICS) {
        // Keep scheduler, thread, and queue events, but avoid logging every
        // uncontended synchronization operation in long-running captures.
        (void)tx_trace_event_filter(TX_TRACE_MUTEX_EVENTS | TX_TRACE_SEMAPHORE_EVENTS |
                                   TX_TRACE_EVENT_FLAGS_EVENTS | TX_TRACE_BYTE_POOL_EVENTS |
                                   TX_TRACE_BLOCK_POOL_EVENTS);
    }
    if (++generation == 0)
        ++generation;
    s->state = PROFILE_RECORDING;
    profile_event(PROFILE_CAT_SYSTEM, PROFILE_CAPTURE, capture, s->core, s->frequency_hz, preset);
    unlock(saved);
    return 0;
#else
    (void)preset;
    (void)capture;
    return 4;
#endif
}
void profile_freeze(uint32_t reason)
{
#if defined(PROTOMATE_PROFILING)
    const unsigned saved = lock();
    if (protomate_profile.status.state == PROFILE_RECORDING) {
        account();
        flush_accounting();
        profile_event(PROFILE_CAT_SYSTEM, PROFILE_CAPTURE, protomate_profile.status.capture, reason, 0, 0);
        protomate_profile.status.frozen_us = profile_time_us();
        if (protomate_profile.status.frozen_us < last_time)
            ++protomate_profile_summary.anchor_epoch;
        protomate_profile_summary.irq_depth = irq_depth;
        protomate_profile.status.reason = reason;
        protomate_profile.status.registry_free = _tx_trace_available_registry_entries;
        (void)tx_trace_disable();
        protomate_profile.status.state = PROFILE_FROZEN;
    }
    unlock(saved);
#else
    (void)reason;
#endif
}
ProfileStatus profile_status(void)
{
    const unsigned saved = lock();
    ProfileStatus result = protomate_profile.status;
    unlock(saved);
    return result;
}
void profile_failure(uint32_t reason, uint32_t operation, uint32_t size, uintptr_t caller)
{
    const unsigned saved = lock();
    ProfileFailure* f = &protomate_profile.status.failure;
    /* Preserve the trigger when later exception handling also fails. */
    if (f->failure_count) {
        unlock(saved);
        return;
    }
    ++f->sequence; /* odd while being written */
    f->time_us = profile_time_us();
    f->operation = operation;
    f->size = size;
    f->caller = (uint32_t)caller;
    f->thread = thread_id();
    ++f->failure_count;
    f->reason = reason;
    ++f->sequence;
    if (reason >= PROFILE_STACK) {
        /* Fatal contexts never touch external memory or invoke ThreadX APIs. */
#if defined(PROTOMATE_PROFILING)
        _tx_trace_buffer_current_ptr = TX_NULL;
        _tx_trace_registry_start_ptr = TX_NULL;
#endif
        protomate_profile.status.reason = reason;
        protomate_profile.status.frozen_us = f->time_us;
        if (protomate_profile.status.state == PROFILE_RECORDING)
            protomate_profile.status.state = PROFILE_FROZEN;
    }
    else {
        profile_event(PROFILE_CAT_SYSTEM, PROFILE_FAILURE, reason, operation, size, (uint32_t)caller);
        profile_freeze(reason);
    }
    unlock(saved);
}
void profile_heap(uint32_t available, uint32_t used)
{
    const unsigned saved = lock();
    if (protomate_profile.status.state == PROFILE_FROZEN) {
        unlock(saved);
        return;
    }
    ProfileStatus* s = &protomate_profile.status;
    s->heap_available = available;
    s->heap_used = used;
    ++s->snapshots;
    if (available < s->sampled_min_free)
        s->sampled_min_free = available;
    profile_event(PROFILE_CAT_SYSTEM, PROFILE_HEAP, available, used, s->sampled_min_free, 0);
    unlock(saved);
}
void profile_stack(uintptr_t thread, const char* name, uint32_t priority, uint32_t size, uint32_t peak)
{
#if defined(PROTOMATE_PROFILING)
    const unsigned saved = lock();
    if (protomate_profile.status.state == PROFILE_RECORDING) {
        CpuThread* cpu = cpu_slot((uint32_t)thread);
        if (cpu) {
            cpu->priority = priority;
            ProfileThread* t = &protomate_profile_summary.threads[cpu - cpu_threads];
            t->priority = priority;
            t->stack_size = size;
            if (peak > t->peak_stack)
                t->peak_stack = peak;
            unsigned i = 0;
            if (name)
                for (; i + 1 < sizeof(t->name) && name[i]; ++i)
                    t->name[i] = name[i];
            t->name[i] = 0;
        }
    }
    unlock(saved);
#else
    (void)thread;
    (void)name;
    (void)priority;
    (void)size;
    (void)peak;
#endif
}
void profile_allocation(uint32_t operation, uint32_t size, uintptr_t address, uintptr_t caller, int failed)
{
    const unsigned saved = lock();
    if (protomate_profile.status.state == PROFILE_FROZEN) {
        unlock(saved);
        return;
    }
    ++protomate_profile.status.allocation_calls;
    protomate_profile.status.last_requested = size;
    if (failed)
        ++protomate_profile.status.allocation_failures;
    profile_event(
      PROFILE_CAT_MEMORY, PROFILE_ALLOCATION_EVENT, operation, size, (uint32_t)address, (uint32_t)caller);
    if (failed)
        profile_failure(PROFILE_ALLOCATION, operation, size, caller);
    unlock(saved);
    (void)address;
}
void profile_thread_switch(uintptr_t thread, uint32_t priority)
{
#if defined(PROTOMATE_PROFILING)
    const unsigned saved = lock();
    account();
    const uint32_t previous = current_thread;
    current_thread = (uint32_t)thread;
    if (protomate_profile.status.state == PROFILE_RECORDING) {
        active_cpu = cpu_slot(current_thread);
        if (active_cpu)
            active_cpu->priority = priority;
        profile_event(PROFILE_CAT_SYSTEM, PROFILE_SCHEDULE, previous, current_thread, priority, 0);
    }
    unlock(saved);
#else
    (void)thread;
    (void)priority;
#endif
}
#if defined(PROTOMATE_PROFILING)
static void trace_irq(uint32_t irq, uint32_t event)
{
    /* Same field layout as ThreadX's native ISR insertion API. Our caller
     * already holds PRIMASK; sharing this body avoids duplicate RAM code. */
    unsigned stack_address = 0;
    profile_tx_insert(event,
                      (uintptr_t)&stack_address,
                      irq,
                      TX_THREAD_GET_SYSTEM_STATE(),
                      _tx_thread_preempt_disable,
                      TX_TRACE_INTERNAL_EVENTS);
}
#endif
void profile_irq_enter(uint32_t irq)
{
#if defined(PROTOMATE_PROFILING)
    const unsigned saved = lock();
    account();
    ++irq_depth;
    if (protomate_profile.status.state == PROFILE_RECORDING && irq != 15 && enabled(PROFILE_CAT_IRQ))
        trace_irq(irq, TX_TRACE_ISR_ENTER);
    unlock(saved);
#else
    (void)irq;
#endif
}
void profile_irq_exit(uint32_t irq)
{
#if defined(PROTOMATE_PROFILING)
    const unsigned saved = lock();
    account();
    if (protomate_profile.status.state == PROFILE_RECORDING && irq != 15 && enabled(PROFILE_CAT_IRQ))
        trace_irq(irq, TX_TRACE_ISR_EXIT);
    if (irq_depth)
        --irq_depth;
    unlock(saved);
#else
    (void)irq;
#endif
}
void profile_service(void)
{
    const uint32_t request = protomate_profile.request;
    if (request != protomate_profile.acknowledge) {
        if (protomate_profile.operation == 1)
            protomate_profile.result =
              (uint32_t)profile_arm(protomate_profile.preset, protomate_profile.capture);
        else if (protomate_profile.operation == 2) {
            profile_freeze(PROFILE_MANUAL);
            protomate_profile.result = 0;
        }
#if defined(PROTOMATE_PROFILING)
        else if (protomate_profile.operation == 3 && protomate_profile.status.state == PROFILE_RECORDING) {
            /* Explicit debugger-only calibration. Each sample masks IRQs only
             * around one insertion, never around the complete batch. */
            for (unsigned i = 0; i < 256; ++i) {
                unsigned saved = lock();
                uint32_t start = profile_cycles();
                profile_measure(62, profile_cycles() - start, 0, PROFILE_CAT_SYSTEM, 0);
                start = profile_cycles();
                profile_event(PROFILE_CAT_SYSTEM, PROFILE_GRAPHICS_STAGE, 0xCA1U, i, 0, 0);
                profile_measure(63, profile_cycles() - start, 0, PROFILE_CAT_SYSTEM, 0);
                unlock(saved);
            }
            protomate_profile.result = 0;
        }
        else if (protomate_profile.operation == 4 && __atomic_load_n(&arm_allowed, __ATOMIC_ACQUIRE)) {
            /* Deliberate allocation failure without touching motor state. The
             * volatile size prevents compile-time malloc diagnostics/folding. */
#if defined(__arm__)
            volatile size_t size = SIZE_MAX;
            void* allocated = malloc(size);
            free(allocated);
            protomate_profile.result = allocated == NULL ? 0U : 1U;
#else
            profile_allocation(1, UINT32_MAX, 0, 0, 1);
            protomate_profile.result = 0;
#endif
        }
#endif
        else
            protomate_profile.result = 1;
        __atomic_store_n(&protomate_profile.acknowledge, request, __ATOMIC_RELEASE);
    }
#if defined(PROTOMATE_PROFILING)
    const uint32_t now = profile_time_us();
    if (protomate_profile.status.state == PROFILE_RECORDING && now - last_anchor >= 1000000U) {
        const unsigned saved = lock();
        // A CLI freeze can preempt this worker between the fast check and
        // taking the lock. Frozen summaries must remain immutable too.
        const uint32_t sample_time = profile_time_us();
        if (protomate_profile.status.state == PROFILE_RECORDING && sample_time - last_anchor >= 1000000U) {
            account();
            flush_accounting();
            if (sample_time < last_time)
                ++protomate_profile_summary.anchor_epoch;
            last_time = last_anchor = sample_time;
            profile_event(PROFILE_CAT_SYSTEM,
                          PROFILE_ANCHOR,
                          protomate_profile.status.capture,
                          protomate_profile_summary.anchor_epoch,
                          sample_time,
                          profile_cycles());
        }
        unlock(saved);
    }
#endif
}
