#include "hal/drivers/factory/step.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/impl/StepGenerator.hpp"

#include "hal/drivers/impl/stm32/StepHardware.hpp"
#include "hal/stm32/InterruptGuard.hpp"
#include <algorithm>

namespace hal::step
{
    auto create(Configuration configuration) -> std::shared_ptr<IStepGenerator>
    {
        using enum gpio::Port;
        const auto& pins{ configuration.pins };
        constexpr std::array channel1{ gpio::Pin{ .port = A, .number = 0 },
                                       gpio::Pin{ .port = A, .number = 5 },
                                       gpio::Pin{ .port = A, .number = 15 } };
        constexpr std::array channel3{ gpio::Pin{ .port = A, .number = 2 },
                                       gpio::Pin{ .port = B, .number = 10 } };
        constexpr std::array channel4{ gpio::Pin{ .port = A, .number = 3 },
                                       gpio::Pin{ .port = B, .number = 11 } };
        if (!std::ranges::contains(channel1, pins[0]) || !std::ranges::contains(channel3, pins[1]) ||
            !std::ranges::contains(channel4, pins[2])) {
            return {};
        }
        std::array<std::shared_ptr<IDigitalOutput>, 3> owners{};
        for (std::size_t i{ 0 }; i < pins.size(); ++i) {
            owners[i] = gpio::create_output({ .pin = pins[i], .pull = gpio::Pull::Down });
            if (!owners[i]) {
                return {};
            }
        }
        auto generator{ std::make_shared<util::StepGenerator>(std::make_unique<stm32::StepHardware>(pins),
                                                              std::move(owners)) };
        const stm32::InterruptGuard lock{};
        stm32::register_step_generator(generator.get());
        return generator;
    }
}
