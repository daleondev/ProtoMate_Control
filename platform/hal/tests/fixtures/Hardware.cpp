// Explicit compatibility hardware for isolated unit tests. Never linked into Application.
#include "LegacyStepHardware.hpp"
#include "hal/drivers/impl/StepGenerator.hpp"
#include "hal/drivers/impl/linux/Uart.hpp"
#include "hal/tests/fixtures/Tmc2209Model.hpp"
namespace hal::util
{
    std::unique_ptr<StepHardware> make_fixture_step() { return std::make_unique<LegacyStepHardware>(); }
    void start_fixture_step(StepHardware& hardware, StepGenerator& generator)
    {
        auto& model{ static_cast<LegacyStepHardware&>(hardware) };
        model.interrupt = [&generator] { generator.service(); };
        model.beginService(generator);
    }
}
namespace hal::device::tmc2209
{
    void configure_fixture_uart(const std::shared_ptr<IUart>& transport)
    {
        std::static_pointer_cast<Uart>(transport)->setExchangeHandler(
          [model = std::make_shared<Tmc2209Model>()](auto tx, auto rx, auto timeout) {
            return model->exchange(tx, rx, timeout);
        });
    }
}
