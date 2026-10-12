#include "resources.hpp"

namespace hal::util
{
    auto resource_allowed(gpio::Pin pin) noexcept -> bool
    {
        return board::resources::allows(executing_core(), pin);
    }

    auto resource_allowed(timer::Peripheral peripheral) noexcept -> bool
    {
        return board::resources::allows(executing_core(), peripheral);
    }
}
