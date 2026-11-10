// CMSIS/HAL register macros use fixed MMIO addresses and C pointer casts.
// NOLINTBEGIN(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
#include "hal/drivers/factory/rtc.hpp"
#include "hal/drivers/impl/stm32/Rtc.hpp"

namespace hal::rtc
{
    auto create() -> std::shared_ptr<IRtc>
    {
        // Hardware is initialized exactly once by M7 before releasing M4.
        // Handles and thread synchronization remain private to each processor.
        static RTC_HandleTypeDef handle{};
        static auto instance{ [] {
            handle.Instance = RTC;
            return std::make_shared<Rtc>(Rtc::Configuration{ handle });
        }() };
        return instance;
    }
}

// NOLINTEND(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
