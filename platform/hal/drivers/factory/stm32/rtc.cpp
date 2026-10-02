#include "hal/drivers/factory/rtc.hpp"

#include "hal/drivers/impl/stm32/Rtc.hpp"
#include "hal/hal.hpp"

#include <memory>

namespace hal::rtc
{
    auto create() -> std::shared_ptr<IRtc>
    {
        static auto rtc{ std::make_shared<Rtc>(Rtc::Configuration{ .handle = hrtc }) };
        return rtc;
    }
}
