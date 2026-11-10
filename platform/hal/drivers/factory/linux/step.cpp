#include "hal/drivers/factory/step.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/impl/StepGenerator.hpp"
#include "simulation/Client.hpp"

#include "hal/drivers/impl/linux/StepHardware.hpp"

namespace hal::util
{
    std::unique_ptr<StepHardware> make_fixture_step() __attribute__((weak));
    void start_fixture_step(StepHardware&, StepGenerator&) __attribute__((weak));
}
namespace hal::step
{
    auto create(Configuration /*configuration*/) -> std::shared_ptr<IStepGenerator>
    {
        using enum gpio::Port;
        const std::array pins{ gpio::Pin{ .port = A, .number = 0U },
                               simulation::client::connected() ? gpio::Pin{ .port = A, .number = 2 }
                                                               : gpio::Pin{ .port = B, .number = 10 },
                               simulation::client::connected() ? gpio::Pin{ .port = A, .number = 3 }
                                                               : gpio::Pin{ .port = B, .number = 11 } };
        std::array<std::shared_ptr<IDigitalOutput>, 3> owners{};
        for (std::size_t i{ 0 }; i < pins.size(); ++i) {
            owners[i] = gpio::create_output({ .pin = pins[i], .pull = gpio::Pull::Down });
            if (!owners[i]) {
                return {};
            }
        }
        std::unique_ptr<util::StepHardware> hardware{};
        if (simulation::client::connected()) {
            hardware = std::make_unique<util::LinuxStepHardware>();
        }
        else if (util::make_fixture_step) {
            hardware = util::make_fixture_step();
        }
        else {
            return {};
        }
        auto* model{ hardware.get() };
        auto generator{ std::make_shared<util::StepGenerator>(std::move(hardware), std::move(owners)) };
        if (simulation::client::connected()) {
            util::LinuxStepHardware::beginService(*generator);
        }
        else if (util::start_fixture_step) {
            util::start_fixture_step(*model, *generator);
        }

        return generator;
    }
}
