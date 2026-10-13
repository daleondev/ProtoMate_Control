#include "hal/drivers/factory/encoder.hpp"

#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/impl/stm32/QuadratureEncoder.hpp"

#include <utility>

namespace
{
    inline constexpr auto ENCODER_B_PIN_NUMBER{ 5U };
}

namespace hal::encoder
{
    auto create(Configuration configuration) -> std::shared_ptr<IQuadratureEncoder>
    {
        using enum gpio::Port;
        if (configuration.timer != timer::Peripheral::Tim3 ||
            configuration.a != gpio::Pin{ .port = B, .number = 4U } ||
            (configuration.b != gpio::Pin{ .port = B, .number = ENCODER_B_PIN_NUMBER } &&
             configuration.b != gpio::Pin{ .port = A, .number = 7U })) {
            return {};
        }
        util::TimerLease lease{ configuration.timer };
        util::TimerLease samplerLease{ timer::Peripheral::Tim6 };
        if (!lease || !samplerLease) {
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
        auto instance{ std::make_shared<QuadratureEncoder>(
            std::move(lease), std::move(samplerLease), std::move(a), std::move(b)) };
        return instance;
    }
}
