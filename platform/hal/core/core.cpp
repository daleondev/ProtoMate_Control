#include "core.hpp"
#include "Catalog.hpp"

namespace hal::core
{
    namespace
    {
        constexpr auto DEVICES = devices_for(util::executing_core());

        template<typename T>
        auto create(std::shared_ptr<T> (*factory)()) -> std::shared_ptr<T>
        {
            return factory ? factory() : nullptr;
        }
    }

    auto create_user_button() -> std::shared_ptr<device::IButton> { return create(DEVICES.user_button); }
    auto create_error_led() -> std::shared_ptr<device::ILed> { return create(DEVICES.error_led); }
    auto create_rtc() -> std::shared_ptr<IRtc> { return create(DEVICES.rtc); }
    auto create_rng() -> std::shared_ptr<IRng> { return create(DEVICES.rng); }
    auto create_status_led() -> std::shared_ptr<device::ILed> { return create(DEVICES.status_led); }
    auto create_intercore_channel() -> std::shared_ptr<IInterCoreChannel> { return create(DEVICES.intercore_channel); }
    auto create_runtime_timer() -> std::shared_ptr<ITimer> { return create(DEVICES.runtime_timer); }
}
