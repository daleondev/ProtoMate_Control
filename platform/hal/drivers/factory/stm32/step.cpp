#include "hal/drivers/impl/stm32/StepHardware.hpp"

namespace hal::stm32
{
    auto makeStepHardware() -> std::unique_ptr<detail::StepHardware>
    {
        return std::make_unique<StepHardware>();
    }
}
