#pragma once
#include "stm32h7xx_hal.h"
#define BOARD_ENABLE_PORT GPIOC
#define BOARD_ENABLE_PIN GPIO_PIN_7
#define BOARD_STEP_PORT GPIOA
#define BOARD_STEP_PINS (GPIO_PIN_0 | GPIO_PIN_2 | GPIO_PIN_3)

// C-compatible startup helper, also included by C++ fault handlers.
// NOLINTNEXTLINE(modernize-redundant-void-arg)
static inline void board_motor_gpio_clocks(void)
{
    // STM32 HAL clock macros contain C MMIO casts and a do-while wrapper.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-do-while,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
    __HAL_RCC_GPIOC_CLK_ENABLE();
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-do-while,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
    __HAL_RCC_GPIOA_CLK_ENABLE();
}
