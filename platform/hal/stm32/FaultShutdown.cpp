#include "FaultShutdown.hpp"

#include "hal/hal.hpp"

#include <bit>
#include <cstdint>

namespace
{
    auto forceOutput(GPIO_TypeDef* port, std::uint32_t pin, bool high) noexcept -> void
    {
        const auto shift{ 2U * std::countr_zero(pin) };
        const auto mask{ 3U << shift };
        // Set the latch before switching from AF/input to push-pull output.
        port->BSRR = high ? pin : pin << 16U;
        port->OTYPER = port->OTYPER & ~pin;
        port->OSPEEDR = port->OSPEEDR & ~mask;
        port->PUPDR = port->PUPDR & ~mask;
        port->MODER = (port->MODER & ~mask) | (1U << shift);
    }
}

namespace hal::stm32
{
    auto shutdownMotionOnFault() noexcept -> void
    {
        __disable_irq();
        if (__HAL_RCC_TIM2_IS_CLK_ENABLED()) {
            TIM2->CR1 = 0U;
            TIM2->DIER = 0U;
            TIM2->CCER = 0U;
        }
        // A pending step DMA transfer can only write a CCR or clear CR1; it
        // cannot restart this timer. Do not wait for DMA or call driver stop().
        __HAL_RCC_GPIOE_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();
        __HAL_RCC_GPIOB_CLK_ENABLE();
        __DSB();
        forceOutput(STEPPERS_EN_N_GPIO_Port, STEPPERS_EN_N_Pin, true);
        forceOutput(M1_STEP_GPIO_Port, M1_STEP_Pin, false);
        forceOutput(M2_STEP_GPIO_Port, M2_STEP_Pin, false);
        forceOutput(M3_STEP_GPIO_Port, M3_STEP_Pin, false);
        __DSB();
    }
}
