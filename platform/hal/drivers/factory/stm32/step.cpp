#include "hal/drivers/factory/step.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/impl/StepGenerator.hpp"

#include "hal/drivers/impl/stm32/StepHardware.hpp"
#include "hal/stm32/InterruptGuard.hpp"

namespace hal::step
{
    auto create(Configuration configuration) -> std::shared_ptr<IStepGenerator>
    {
        using enum gpio::Port;
        const auto& pins = configuration.pins;
        if ((pins[0] != gpio::Pin{A, 0} && pins[0] != gpio::Pin{A, 5} && pins[0] != gpio::Pin{A, 15}) ||
            (pins[1] != gpio::Pin{A, 2} && pins[1] != gpio::Pin{B, 10}) ||
            (pins[2] != gpio::Pin{A, 3} && pins[2] != gpio::Pin{B, 11})) { return {}; }
        std::array<std::shared_ptr<IDigitalOutput>, 3> owners;
        for (std::size_t i = 0; i < pins.size(); ++i) {
            owners[i] = gpio::create_output({ .pin = pins[i], .pull = gpio::Pull::Down });
            if (!owners[i]) {
                return {};
            }
        }
        auto generator{ std::make_shared<util::StepGenerator>(std::make_unique<stm32::StepHardware>(pins),
                                                              std::move(owners)) };
        const stm32::InterruptGuard lock;
        stm32::register_step_generator(generator.get());
        return generator;
    }
}
