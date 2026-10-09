#pragma once

#include "hal/drivers/itf/ITimer.hpp"
#include <memory>

namespace hal::timer
{
    // TIM5 is the shared runtime timer. Other timers are reserved for the HAL
    // tick, step generator or the exclusive PWM/encoder factories.
    [[nodiscard]] auto create(Configuration configuration) -> std::shared_ptr<ITimer>;
}
