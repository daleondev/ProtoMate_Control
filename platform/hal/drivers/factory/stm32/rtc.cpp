#include "hal/drivers/factory/rtc.hpp"

namespace hal::rtc
{
    auto create() -> std::shared_ptr<IRtc>
    {
        // No core owns this shared peripheral yet.
        return {};
    }
}
