#include "hal/drivers/factory/encoder.hpp"

#include "hal/drivers/factory/gpio.hpp"
#if defined(HAL_PLATFORM_STM32)
#include "hal/drivers/impl/stm32/QuadratureEncoder.hpp"
#else
#include "hal/drivers/impl/linux/QuadratureEncoder.hpp"
#include "hal/linux/Mutex.hpp"
#endif

#include <utility>

namespace hal::encoder
{
#if defined(HAL_PLATFORM_LINUX)
    namespace
    {
        linux::Mutex owner_mutex;
        std::weak_ptr<QuadratureEncoder> owner;
    }
    auto simulatedEncoder(timer::Peripheral peripheral) -> std::shared_ptr<QuadratureEncoder>
    {
        const std::scoped_lock lock{ owner_mutex };
        return peripheral == timer::Peripheral::Tim3 ? owner.lock() : nullptr;
    }
#endif
    auto create(Configuration configuration) -> std::shared_ptr<IQuadratureEncoder>
    {
        using enum gpio::Port;
        if (configuration.timer != timer::Peripheral::Tim3 || configuration.a != gpio::Pin{ B, 4U } ||
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
        auto instance{ std::make_shared<QuadratureEncoder>(std::move(lease), std::move(a), std::move(b)) };
#if defined(HAL_PLATFORM_LINUX)
        const std::scoped_lock lock{ owner_mutex };
        owner = instance;
#endif
        return instance;
    }
}
