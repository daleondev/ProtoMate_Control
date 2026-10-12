#include "core.hpp"

#include "hal/board/board.hpp"

namespace hal::core
{
    auto create_user_button() -> std::shared_ptr<device::IButton> { return board::create_button(board::ButtonId::User); }
    auto create_error_led() -> std::shared_ptr<device::ILed> { return board::create_led(board::LedId::Red); }
    auto create_rtc() -> std::shared_ptr<IRtc> { return board::create_rtc(id()); }
    auto create_rng() -> std::shared_ptr<IRng> { return board::create_rng(id()); }

    auto create_status_led() -> std::shared_ptr<device::ILed>
    {
        return board::create_status_led(id());
    }

    auto create_intercore_channel() -> std::shared_ptr<IInterCoreChannel>
    {
        return board::create_intercore_channel(id());
    }

    auto create_runtime_timer() -> std::shared_ptr<ITimer>
    {
        return board::create_runtime_timer(id());
    }
}
