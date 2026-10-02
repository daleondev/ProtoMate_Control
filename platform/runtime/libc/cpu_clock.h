#ifndef RUNTIME_CPU_CLOCK_H
#define RUNTIME_CPU_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* Sample at least once per 32-bit hardware counter revolution. Unsigned
 * subtraction handles a wrap between samples, including while idle. */
typedef struct
{
    uint64_t execution_cycles;
    uint64_t elapsed_cycles;
    uint32_t last_cycle;
    bool running;
} runtime_cpu_clock_counter;

static inline void runtime_cpu_clock_update(runtime_cpu_clock_counter* counter, uint32_t now)
{
    const uint32_t elapsed = now - counter->last_cycle;
    counter->elapsed_cycles += elapsed;
    if (counter->running) {
        counter->execution_cycles += elapsed;
    }
    counter->last_cycle = now;
}

static inline uint64_t runtime_cpu_clock_ticks(uint64_t cycles, uint32_t frequency, uint32_t rate)
{
    return (cycles / frequency) * rate + ((cycles % frequency) * rate) / frequency;
}

#ifdef __cplusplus
extern "C" {
#endif

void runtime_cpu_clock_initialize(void);
void runtime_cpu_clock_thread_enter(void);
void runtime_cpu_clock_thread_exit(void);
void runtime_cpu_clock_sample(void);

#ifdef __cplusplus
}
#endif

#endif
