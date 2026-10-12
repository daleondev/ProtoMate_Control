#include "hal/board/board.hpp"
#include "hal/devices/impl/Led.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/factory/timer.hpp"

namespace hal::board
{
    auto create_led(LedId id) -> std::shared_ptr<device::ILed>
    {
#if defined(CORE_CM4)
        constexpr auto owned_id = LedId::Green;
        constexpr gpio::Pin pin{gpio::Port::B, 0};
#else
        constexpr auto owned_id = LedId::Yellow;
        constexpr gpio::Pin pin{gpio::Port::E, 1};
#endif
        if (id != owned_id) {
            return {};
        }
        static auto led = std::make_shared<device::Led>(
          gpio::create_output({.pin = pin, .initial_level = gpio::Level::Low}), gpio::Level::High);
        return led;
    }

    auto create_runtime_timer() -> std::shared_ptr<ITimer>
    {
#if defined(CORE_CM4)
        return timer::create({.peripheral = timer::Peripheral::Tim5});
#else
        return timer::create({.peripheral = timer::Peripheral::Tim12});
#endif
    }
}
