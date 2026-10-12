#pragma once

#include "hal/drivers/itf/GpioTypes.hpp"
#include "hal/drivers/itf/ITimer.hpp"

namespace hal::util
{
    // The selected board supplies this immutable policy. Factories query it
    // before claiming a resource or accessing registers; no driver depends
    // on board/core factory implementations.
    [[nodiscard]] auto resource_allowed(gpio::Pin pin) noexcept -> bool;
    [[nodiscard]] auto resource_allowed(timer::Peripheral peripheral) noexcept -> bool;
}
