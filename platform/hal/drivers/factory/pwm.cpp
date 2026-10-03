#include "hal/drivers/factory/pwm.hpp"

#include "hal/drivers/factory/gpio.hpp"
#if defined(HAL_PLATFORM_STM32)
#include "hal/drivers/impl/stm32/PwmOutput.hpp"
#else
#include "hal/drivers/impl/linux/PwmOutput.hpp"
#endif

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
            Route{ { 1U, 1U, { E, 9U } }, 1U },
            Route{ { 4U, 3U, { D, 14U } }, 2U },
            Route{ { 8U, 1U, { C, 6U } }, 3U },
        };
        for (const auto& route : routes) {
            if (configuration.timer != route.configuration.timer ||
                configuration.channel != route.configuration.channel ||
                configuration.pin != route.configuration.pin) {
                continue;
            }
            detail::TimerLease lease{ configuration.timer };
            if (!lease) {
                return {};
            }
            // Own a real low output while stopped. The STM32 driver selects AF
            // with an inactive timer output before the first pulse.
            auto pin{ gpio::createOutput({ .pin = configuration.pin }) };
            if (!pin) {
                return {};
            }
            return std::make_shared<PwmOutput>(
              configuration, route.alternate, std::move(lease), std::move(pin));
        }
        return {};
    }
}
