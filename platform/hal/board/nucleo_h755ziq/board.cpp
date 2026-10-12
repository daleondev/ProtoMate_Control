#include "hal/board/board.hpp"
#include "hal/devices/impl/Led.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/factory/timer.hpp"
#include "resources.hpp"

namespace hal::board
{
    namespace
    {
        auto make_led(gpio::Pin pin) -> std::shared_ptr<device::ILed>
        {
            auto output = gpio::create_output({.pin = pin, .initial_level = gpio::Level::Low});
            if (!output) {
                return {};
            }
            return std::make_shared<device::Led>(std::move(output), gpio::Level::High);
        }
    }

    auto create_led(LedId id) -> std::shared_ptr<device::ILed>
    {
        switch (id) {
            case LedId::Green: {
                if (!util::resource_allowed(resources::M4_STATUS_LED)) {
                    return {};
                }
                static auto led = make_led(resources::M4_STATUS_LED);
                return led;
            }
            case LedId::Yellow: {
                if (!util::resource_allowed(resources::M7_STATUS_LED)) {
                    return {};
                }
                static auto led = make_led(resources::M7_STATUS_LED);
                return led;
            }
            default:
                return {};
        }
    }

    auto create_status_led(util::Core owner) -> std::shared_ptr<device::ILed>
    {
        if (owner != util::executing_core()) {
            return {};
        }
        switch (owner) {
            case util::Core::M4: return create_led(LedId::Green);
            case util::Core::M7: return create_led(LedId::Yellow);
            default: return {};
        }
    }

    auto create_runtime_timer(util::Core owner) -> std::shared_ptr<ITimer>
    {
        if (owner != util::executing_core()) {
            return {};
        }
        switch (owner) {
            case util::Core::M4: return timer::create({.peripheral = resources::M4_RUNTIME_TIMER});
            case util::Core::M7: return timer::create({.peripheral = resources::M7_RUNTIME_TIMER});
            default: return {};
        }
    }
}
