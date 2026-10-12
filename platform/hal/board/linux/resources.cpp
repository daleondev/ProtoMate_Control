#include "hal/util/ResourceAccess.hpp"

namespace hal::util
{
    // Host tests use virtual resources, without claiming physical MCU pins.
    auto resource_allowed(gpio::Pin) noexcept -> bool { return true; }
    auto resource_allowed(timer::Peripheral) noexcept -> bool { return true; }
}
