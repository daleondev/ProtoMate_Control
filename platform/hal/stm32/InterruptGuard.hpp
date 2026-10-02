#pragma once

#include "hal/hal.hpp"

#include <cstdint>

#if !defined(HAL_PLATFORM_STM32)
#error "The STM32 interrupt guard is only available on STM32 targets"
#endif

namespace hal::stm32
{
    class InterruptGuard final
    {
      public:
        InterruptGuard() noexcept
          : m_previousPrimask{ __get_PRIMASK() }
        {
            __disable_irq();
            __DMB();
        }

        ~InterruptGuard()
        {
            __DMB();
            __set_PRIMASK(m_previousPrimask);
        }

        InterruptGuard(const InterruptGuard&) = delete;
        InterruptGuard& operator=(const InterruptGuard&) = delete;
        InterruptGuard(InterruptGuard&&) = delete;
        InterruptGuard& operator=(InterruptGuard&&) = delete;

      private:
        std::uint32_t m_previousPrimask;
    };
}
