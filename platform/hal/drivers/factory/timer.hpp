#pragma once

#include "hal/drivers/itf/ITimer.hpp"
#include <memory>

namespace hal::timer
{
    // Only timers assigned to this core's general timer driver are available.
    // The board selects its runtime counter through create_runtime_timer().
    [[nodiscard]] auto create(Configuration configuration) -> std::shared_ptr<ITimer>;
}
