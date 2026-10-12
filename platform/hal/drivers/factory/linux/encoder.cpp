#include "hal/drivers/factory/encoder.hpp"

#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/impl/linux/QuadratureEncoder.hpp"
#include "hal/linux/Mutex.hpp"


#include <utility>

namespace
{
    inline constexpr auto ENCODER_B_PIN_NUMBER{ 5U };
}

namespace hal::encoder
{
    namespace
    {
        auto owner_mutex() -> linux::Mutex&
        {
            static linux::Mutex value;
            return value;
        }
        auto owner() -> std::weak_ptr<QuadratureEncoder>&
        {
            static std::weak_ptr<QuadratureEncoder> value;
            return value;
        }
    }
    auto simulated_encoder(timer::Peripheral peripheral) -> std::shared_ptr<QuadratureEncoder>
    {
        const std::scoped_lock lock{ owner_mutex() };
        return peripheral == timer::Peripheral::Tim3 ? owner().lock() : nullptr;
    }

    auto create(Configuration configuration) -> std::shared_ptr<IQuadratureEncoder>
    {
        using enum gpio::Port;
        if (configuration.timer != timer::Peripheral::Tim3 ||
            configuration.a != gpio::Pin{ .port = B, .number = 4U } ||
            configuration.b != gpio::Pin{ .port = B, .number = ENCODER_B_PIN_NUMBER }) {
            return {};
        }
        util::TimerLease lease{ configuration.timer };
        if (!lease) {
            return {};
        }
        auto a{ gpio::create_input({ .pin = configuration.a, .alternate_function = 2U }) };
        if (!a) {
            return {};
        }
        auto b{ gpio::create_input({ .pin = configuration.b, .alternate_function = 2U }) };
        if (!b) {
            return {};
        }
        auto instance{ std::make_shared<QuadratureEncoder>(std::move(lease), std::move(a), std::move(b)) };
        const std::scoped_lock lock{ owner_mutex() };
        owner() = instance;

        return instance;
    }
}
