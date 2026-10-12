#include "hal/drivers/factory/step.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/impl/StepGenerator.hpp"

#if defined(HAL_PLATFORM_STM32)
#include "hal/drivers/impl/stm32/StepHardware.hpp"
#include "hal/stm32/InterruptGuard.hpp"
#else
#include "hal/drivers/impl/linux/StepHardware.hpp"
#endif

namespace hal::step
{
    auto create() -> std::shared_ptr<IStepGenerator>
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
#if defined(HAL_PLATFORM_STM32)
        auto generator{ std::make_shared<util::StepGenerator>(std::make_unique<stm32::StepHardware>(),
                                                              std::move(owners)) };
        const stm32::InterruptGuard lock;
        stm32::register_step_generator(generator.get());
#else
        auto hardware{ std::make_unique<util::LinuxStepHardware>() };
        auto* model{ hardware.get() };
        auto generator{ std::make_shared<util::StepGenerator>(std::move(hardware), std::move(owners)) };
        model->interrupt = [instance = generator.get()] { instance->service(); };
        model->beginService(*generator);
#endif
        return generator;
    }
}
