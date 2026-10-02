#include "hal/drivers/factory/rtc.hpp"

#include "hal/drivers/impl/linux/Rtc.hpp"

#include <memory>

namespace hal::rtc
{
    auto create() -> std::shared_ptr<IRtc>
    {
        static auto rtc{ std::make_shared<Rtc>() };
        return rtc;
    }
}
