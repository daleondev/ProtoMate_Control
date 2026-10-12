#include "hal/hal.hpp"
#include <tx_api.h>

#include <cstdint>

extern "C" {
    extern void* _tx_thread_system_stack_ptr;
    extern std::uint32_t g_pfnVectors[];
    void _tx_timer_interrupt();
    void _tx_execution_isr_enter();
    void _tx_execution_isr_exit();
}

namespace
{
    // Each image has its own state and core-local SysTick/NVIC registers.
    volatile bool kernel_tick_ready{};
}

extern "C" void _tx_initialize_low_level()
{
    __disable_irq();
    _tx_thread_system_stack_ptr = reinterpret_cast<void*>(g_pfnVectors[0]);
    SCB->VTOR = reinterpret_cast<std::uintptr_t>(g_pfnVectors);
    NVIC_SetPriority(SVCall_IRQn, 15);
    NVIC_SetPriority(PendSV_IRQn, 15);
    SysTick->CTRL = 0;
    SysTick->LOAD = SystemCoreClock / TX_TIMER_TICKS_PER_SECOND - 1;
    SysTick->VAL = 0;
    NVIC_SetPriority(SysTick_IRQn, 4);
    kernel_tick_ready = true;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;
}

extern "C" void SysTick_Handler()
{
    // Before the scheduler starts, HAL initialization still needs its tick.
    if (!kernel_tick_ready) {
        HAL_IncTick();
        return;
    }
    _tx_execution_isr_enter();
    HAL_IncTick();
    _tx_timer_interrupt();
    _tx_execution_isr_exit();
}
