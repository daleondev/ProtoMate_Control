#include "hal/drivers/factory/encoder.hpp"

#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/impl/stm32/QuadratureEncoder.hpp"

#include <utility>

namespace
{
    inline constexpr auto ENCODER_B_PIN_NUMBER{ 5U };
    inline constexpr auto ENCODER_B_ALTERNATE_PIN_NUMBER{ 7U };
}

namespace hal::encoder
{
    auto create(Configuration configuration) -> std::shared_ptr<IQuadratureEncoder>
    {
        using enum gpio::Port;
        if (configuration.timer != timer::Peripheral::Tim3 ||
            configuration.a != gpio::Pin{ .port = B, .number = 4U } ||
            (configuration.b != gpio::Pin{ .port = B, .number = ENCODER_B_PIN_NUMBER } &&
             configuration.b != gpio::Pin{ .port = A, .number = ENCODER_B_ALTERNATE_PIN_NUMBER })) {
            return {};
        }
        util::TimerLease lease{ configuration.timer };
        util::TimerLease sampler_lease{ timer::Peripheral::Tim6 };
        if (!lease || !sampler_lease) {
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
          std::move(lease), std::move(sampler_lease), std::move(a), std::move(b)) };
        return instance;
    }
}
