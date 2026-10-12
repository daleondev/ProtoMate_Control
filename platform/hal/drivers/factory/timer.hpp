#pragma once

#include "hal/drivers/itf/ITimer.hpp"
#include <memory>

namespace hal::timer
{
    // Only timers assigned to this core's general timer driver are available.
    // hal::core selects its runtime counter through the board mapping.
    [[nodiscard]] auto create(Configuration configuration) -> std::shared_ptr<ITimer>;
}
