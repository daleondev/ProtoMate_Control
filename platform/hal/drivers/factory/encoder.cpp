#include "hal/drivers/factory/encoder.hpp"

#include "hal/drivers/factory/gpio.hpp"
#if defined(HAL_PLATFORM_STM32)
#include "hal/drivers/impl/stm32/QuadratureEncoder.hpp"
#else
#include "hal/drivers/impl/linux/QuadratureEncoder.hpp"
#endif

#include <utility>

namespace hal::encoder
{
    auto create(Configuration configuration) -> std::shared_ptr<IQuadratureEncoder>
    {
        using enum gpio::Port;
        if (configuration.timer != 3U || configuration.a != gpio::Pin{ B, 4U } ||
            configuration.b != gpio::Pin{ B, 5U }) {
            return {};
        }
        detail::TimerLease lease{ configuration.timer };
        if (!lease) {
            return {};
        }
        auto a{ gpio::createInput({ .pin = configuration.a, .alternate_function = 2U }) };
        if (!a) {
            return {};
        }
        auto b{ gpio::createInput({ .pin = configuration.b, .alternate_function = 2U }) };
        if (!b) {
            return {};
        }
        return std::make_shared<QuadratureEncoder>(std::move(lease), std::move(a), std::move(b));
    }
}
