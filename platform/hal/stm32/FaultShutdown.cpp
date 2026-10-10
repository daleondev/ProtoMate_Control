// CMSIS/HAL macros intentionally use C casts, fixed MMIO addresses and do-while wrappers.
// The line-specific exceptions below preserve the vendor register interface.
#include "FaultShutdown.hpp"

#include "hal/hal.hpp"

#include <bit>
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
        __HAL_RCC_GPIOE_CLK_ENABLE(); // NOLINT(cppcoreguidelines-avoid-do-while,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        __HAL_RCC_GPIOA_CLK_ENABLE(); // NOLINT(cppcoreguidelines-avoid-do-while,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        __HAL_RCC_GPIOB_CLK_ENABLE(); // NOLINT(cppcoreguidelines-avoid-do-while,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        __DSB();
        force_output(STEPPERS_EN_N_GPIO_Port, // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
                     STEPPERS_EN_N_Pin,
                     true); // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        force_output(M1_STEP_GPIO_Port, // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
                     M1_STEP_Pin,
                     false); // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        force_output(M2_STEP_GPIO_Port, // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
                     M2_STEP_Pin,
                     false); // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        force_output(M3_STEP_GPIO_Port, // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
                     M3_STEP_Pin,
                     false); // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        __DSB();
    }
}
