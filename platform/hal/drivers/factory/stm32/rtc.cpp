#include "hal/drivers/factory/rtc.hpp"

#if !defined(HAL_BOARD_NUCLEO_H755ZIQ)
#include "hal/drivers/impl/stm32/Rtc.hpp"
#endif
#include "hal/hal.hpp"

#include <memory>

namespace hal::rtc
{
    auto create() -> std::shared_ptr<IRtc>
    {
#if defined(HAL_BOARD_NUCLEO_H755ZIQ)
        // The initial H755 profile does not assign the shared RTC to either core.
        return {};
#else
        static auto rtc{ std::make_shared<Rtc>(Rtc::Configuration{ .handle = hrtc }) };
        return rtc;
#endif
    }
}
