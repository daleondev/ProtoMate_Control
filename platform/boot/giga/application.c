#include "platform.h"
extern uint32_t g_pfnVectors[];
extern void __libc_init_array(void);
extern int main(int, char **);

/* The validated internal bootstrap has already loaded all RAM sections. */
void giga_start(void)
{
    SCB->CPACR |= 0xFu << 20;
    __DSB(); __ISB();
    SystemCoreClockUpdate();
#if defined(CORE_CM7)
    const volatile GigaHandoff *h = (const volatile GigaHandoff *)GIGA_HANDOFF_ADDRESS;
    giga_configure_mpu(h->m4_flags & GIGA_RAM_DEBUG);
#endif
    /* M4 inherits its final MPU configuration from the internal bootstrap.
       Disabling it here would make RAM-debug's SDRAM instruction fetches XN. */
    SCB->VTOR = (uint32_t)g_pfnVectors;
#if defined(CORE_CM7)
    SCB_EnableICache();
    SCB_EnableDCache();
#endif
    __DSB(); __ISB();
    __libc_init_array();
    __enable_irq();
    (void)main(0, 0);
    for (;;) { __WFI(); }
}

void giga_release_m4(void)
{
#if defined(CORE_CM7)
    volatile GigaHandoff *h = (volatile GigaHandoff *)GIGA_HANDOFF_ADDRESS;
    h->magic = GIGA_HANDOFF_MAGIC;
    __DMB();
    if (HAL_HSEM_FastTake(0) != HAL_OK) { Error_Handler(); }
    HAL_HSEM_Release(0, 0);
#endif
}

/* Arm EHABI PREL31 cannot span QSPI and internal instruction SRAM. Select
   the matching sorted table through GCC's supported bare-metal hook. */
extern uint32_t __fast_start[], __fast_end[], __fast_exidx_start[], __fast_exidx_end[];
extern uint32_t __exidx_start[], __exidx_end[];
uintptr_t __gnu_Unwind_Find_exidx(uintptr_t pc, int *count)
{
    if (pc >= (uintptr_t)__fast_start && pc < (uintptr_t)__fast_end) {
        *count = (__fast_exidx_end - __fast_exidx_start) / 2;
        return (uintptr_t)__fast_exidx_start;
    }
    *count = (__exidx_end - __exidx_start) / 2;
    return (uintptr_t)__exidx_start;
}
