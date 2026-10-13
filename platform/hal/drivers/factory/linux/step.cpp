#include "hal/drivers/factory/step.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/impl/StepGenerator.hpp"

#include "hal/drivers/impl/linux/StepHardware.hpp"


namespace hal::step
{
    auto create(Configuration /*configuration*/) -> std::shared_ptr<IStepGenerator>
    {
        using enum gpio::Port;
        constexpr std::array pins{ gpio::Pin{ .port = A, .number = 0U },
                                   gpio::Pin{ .port = B, .number = 10U },
                                   gpio::Pin{ .port = B, .number = 11U } };
        std::array<std::shared_ptr<IDigitalOutput>, 3> owners;
        for (std::size_t i = 0; i < pins.size(); ++i) {
            owners[i] = gpio::create_output({ .pin = pins[i], .pull = gpio::Pull::Down });
            if (!owners[i]) {
                return {};
            }
        }
        auto hardware{ std::make_unique<util::LinuxStepHardware>() };
        auto* model{ hardware.get() };
        auto generator{ std::make_shared<util::StepGenerator>(std::move(hardware), std::move(owners)) };
        model->interrupt = [instance = generator.get()] { instance->service(); };
        model->beginService(*generator);

        return generator;
    }
}
