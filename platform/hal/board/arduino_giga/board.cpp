#include "hal/board/board.hpp"
#include "hal/drivers/factory/intercore.hpp"
#include "hal/drivers/factory/rtc.hpp"
#include "hal/drivers/factory/rng.hpp"
#include "hal/devices/impl/Led.hpp"
#include "hal/devices/impl/Button.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/factory/timer.hpp"

namespace hal::board
{
    auto create_rtc() -> std::shared_ptr<IRtc> { return rtc::create(); }
    auto create_rng() -> std::shared_ptr<IRng> { return rng::create(); }
    auto create_intercore_channel() -> std::shared_ptr<IInterCoreChannel> { return intercore::create(); }
    auto create_timer(timer::Peripheral peripheral) -> std::shared_ptr<ITimer>
    {
        return timer::create({.peripheral = peripheral});
    }

    namespace
    {
        constexpr gpio::Pin GREEN_LED{gpio::Port::J, 13};
        constexpr gpio::Pin BLUE_LED{gpio::Port::E, 3};
        constexpr gpio::Pin RED_LED{gpio::Port::I, 12};
        constexpr gpio::Pin USER_BUTTON{gpio::Port::C, 13};

        auto make_led(gpio::Pin pin) -> std::shared_ptr<device::ILed>
        {
            auto output = gpio::create_output({.pin = pin, .initial_level = gpio::Level::High});
            if (!output) {
                return {};
            }
            return std::make_shared<device::Led>(std::move(output), gpio::Level::Low);
        }
    }

    auto create_led(LedId id) -> std::shared_ptr<device::ILed>
    {
        switch (id) {
            case LedId::Green: {
                static auto led = make_led(GREEN_LED);
                return led;
            }
            case LedId::Blue: {
                static auto led = make_led(BLUE_LED);
                return led;
            }
            case LedId::Red: {
                static auto led = make_led(RED_LED);
                return led;
            }
            default:
                return {};
        }
    }

    auto create_button(ButtonId id) -> std::shared_ptr<device::IButton>
    {
        if (id != ButtonId::User) { return {}; }
        static auto button = []() -> std::shared_ptr<device::IButton> {
            auto input = gpio::create_input({.pin = USER_BUTTON,
                                             .pull = gpio::Pull::None,
                                             .edge = gpio::Edge::Both});
            if (!input) { return {}; }
            return std::make_shared<device::Button>(std::move(input), gpio::Level::High);
        }();
        return button;
    }
}
