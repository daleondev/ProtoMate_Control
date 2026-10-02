#include "cpu_clock.h"

#include "hal/hal.hpp"

#include <errno.h>
#include <sys/times.h>
#include <time.h>

static runtime_cpu_clock_counter counter;
static uint32_t counter_frequency;

void runtime_cpu_clock_initialize(void)
{
    const uint32_t posture = __get_PRIMASK();
    __disable_irq();
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    counter = (runtime_cpu_clock_counter){ .last_cycle = DWT->CYCCNT };
    counter_frequency = SystemCoreClock;
    __set_PRIMASK(posture);
}

static void set_running(bool running)
{
    const uint32_t posture = __get_PRIMASK();
    __disable_irq();
    runtime_cpu_clock_update(&counter, DWT->CYCCNT);
    counter.running = running;
    __set_PRIMASK(posture);
}

void runtime_cpu_clock_thread_enter(void) { set_running(true); }
void runtime_cpu_clock_thread_exit(void) { set_running(false); }

void runtime_cpu_clock_sample(void)
{
    const uint32_t posture = __get_PRIMASK();
    __disable_irq();
    runtime_cpu_clock_update(&counter, DWT->CYCCNT);
    __set_PRIMASK(posture);
}

clock_t _times(struct tms* value)
{
    if (value == NULL) {
        errno = EFAULT;
        return (clock_t)-1;
    }

    const uint32_t posture = __get_PRIMASK();
    __disable_irq();
    runtime_cpu_clock_update(&counter, DWT->CYCCNT);
    const runtime_cpu_clock_counter snapshot = counter;
    const uint32_t frequency = counter_frequency;
    __set_PRIMASK(posture);

    if (frequency == 0U) {
        errno = EAGAIN;
        return (clock_t)-1;
    }

    /* Newlib's Arm ABI uses unsigned 32-bit clock_t and CLOCKS_PER_SEC=100.
     * Preserve its units instead of confusing scheduler ticks with CPU time.
     * Keep accumulation at cycle precision so short runs are not lost. */
    _Static_assert(sizeof(clock_t) == sizeof(uint32_t) && (clock_t)-1 > 0, "Arm clock_t ABI changed");
    const uint64_t execution = runtime_cpu_clock_ticks(snapshot.execution_cycles, frequency, CLOCKS_PER_SEC);
    const uint64_t elapsed = runtime_cpu_clock_ticks(snapshot.elapsed_cycles, frequency, CLOCKS_PER_SEC);
    if (execution >= UINT32_MAX || elapsed >= UINT32_MAX) {
        errno = EOVERFLOW;
        return (clock_t)-1;
    }
    *value = (struct tms){ .tms_utime = (clock_t)execution };
    return (clock_t)elapsed;
}
