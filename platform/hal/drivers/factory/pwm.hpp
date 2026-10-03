#pragma once

#include "hal/drivers/itf/IGpio.hpp"
#include "hal/drivers/itf/IPwmOutput.hpp"

#include <cstdint>
#include <memory>

namespace hal::pwm
{
    struct Configuration
    {
        std::uint8_t timer;
        std::uint8_t channel; // 1-based channel number, not an STM32 HAL constant.
        gpio::Pin pin;
    };

    // Supported routes: TIM1_CH1/PE9, TIM4_CH3/PD14, TIM8_CH1/PC6.
    // Reserves the entire timer and the pin until the last owner releases it.
    // Returns null for unsupported routes or already-owned resources.
    [[nodiscard]] auto create(Configuration configuration) -> std::shared_ptr<IPwmOutput>;
}
