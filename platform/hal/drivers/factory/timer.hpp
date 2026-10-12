#pragma once

#include "hal/drivers/itf/ITimer.hpp"
#include <memory>

namespace hal::timer
{
    // Select an initialized general-purpose timer. Hardware configuration is
    // established by platform startup; processor ownership belongs to hal::core.
    [[nodiscard]] auto create(Configuration configuration) -> std::shared_ptr<ITimer>;
}
