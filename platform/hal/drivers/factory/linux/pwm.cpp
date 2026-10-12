#include "hal/drivers/factory/pwm.hpp"

#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/impl/linux/PwmOutput.hpp"


#include <array>
#include <utility>

namespace hal::pwm
{
    auto create(Configuration configuration) -> std::shared_ptr<IPwmOutput>
    {
        struct Route
        {
            Configuration configuration;
            std::uint8_t alternate;
        };
        using enum gpio::Port;
        constexpr std::array routes{
            Route{ .configuration = { .timer = hal::timer::Peripheral::Tim1,
                                      .channel = hal::timer::Channel::Channel1,
                                      .pin = { .port = E, .number = 9U } },
                   .alternate = 1U },
            Route{ .configuration = { .timer = hal::timer::Peripheral::Tim4,
                                      .channel = hal::timer::Channel::Channel3,
                                      .pin = { .port = D, .number = 14U } },
                   .alternate = 2U },
            Route{ .configuration = { .timer = hal::timer::Peripheral::Tim8,
                                      .channel = hal::timer::Channel::Channel1,
                                      .pin = { .port = C, .number = 6U } },
                   .alternate = 3U },
        };
        for (const auto& route : routes) {
            if (configuration.timer != route.configuration.timer ||
                configuration.channel != route.configuration.channel ||
                configuration.pin != route.configuration.pin) {
                continue;
            }
            util::TimerLease lease{ configuration.timer };
            if (!lease) {
                return {};
            }
            // Own a real low output while stopped. The STM32 driver selects AF
            // with an inactive timer output before the first pulse.
            auto pin{ gpio::create_output({ .pin = configuration.pin }) };
            if (!pin) {
                return {};
            }
            return std::make_shared<PwmOutput>(
              configuration, route.alternate, std::move(lease), std::move(pin));
        }
        return {};
    }
}
