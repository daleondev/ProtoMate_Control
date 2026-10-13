// CMSIS/HAL macros intentionally use C casts, fixed MMIO addresses and do-while wrappers.
// The line-specific exceptions below preserve the vendor register interface.
#include "FaultShutdown.hpp"

#include "hal/hal.hpp"
#include "hal/board/arduino_giga/pins.h"

#include <bit>
#include <initializer_list>
#include <cstdint>

namespace
{
    inline constexpr auto GPIO_RESET_SHIFT{ 16U };
    auto force_output(GPIO_TypeDef* port, std::uint32_t pin, bool high) noexcept -> void
    {
        const auto shift{ 2U * std::countr_zero(pin) };
        const auto mask{ 3U << shift };
        // Set the latch before switching from AF/input to push-pull output.
        port->BSRR = high ? pin : pin << GPIO_RESET_SHIFT;
        port->OTYPER = port->OTYPER & ~pin;
        port->OSPEEDR = port->OSPEEDR & ~mask;
        port->PUPDR = port->PUPDR & ~mask;
        port->MODER = (port->MODER & ~mask) | (1U << shift);
    }
}

namespace hal::stm32
{
    auto shutdown_motion_on_fault() noexcept -> void
    {
        __disable_irq();
        if (
          __HAL_RCC_TIM2_IS_CLK_ENABLED()) { // NOLINT(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            TIM2->CR1 = 0U;  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            TIM2->DIER = 0U; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            TIM2->CCER = 0U; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        }
        // A pending step DMA transfer can only write a CCR or clear CR1; it
        // cannot restart this timer. Do not wait for DMA or call driver stop().
        BOARD_MOTOR_GPIO_CLOCKS();
        __DSB();
        force_output(BOARD_ENABLE_PORT, BOARD_ENABLE_PIN, true);
        std::uint32_t pins = BOARD_STEP_PINS;
        while (pins) {
            const auto pin = 1U << std::countr_zero(pins);
            force_output(BOARD_STEP_PORT, pin, false);
            pins &= ~pin;
        }
        __DSB();
    }
}
