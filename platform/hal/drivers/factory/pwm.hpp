#pragma once

#include "hal/drivers/itf/IPwmOutput.hpp"

#include <memory>

namespace hal::pwm
{
    // Supported routes: TIM1_CH1/PE9, TIM4_CH3/PD14, TIM8_CH1/PC6.
    // Reserves the entire timer and the pin until the last owner releases it.
    // Returns null for unsupported routes or already-owned resources.
    [[nodiscard]] auto create(Configuration configuration) -> std::shared_ptr<IPwmOutput>;
}
