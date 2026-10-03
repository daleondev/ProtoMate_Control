#pragma once

#include "hal/drivers/detail/StepHardware.hpp"
#include <memory>

namespace hal::detail
{
    class StepGenerator;
}
namespace hal::stm32
{
    [[nodiscard]] auto makeStepHardware() -> std::unique_ptr<detail::StepHardware>;
    // Registration is protected by PRIMASK, before interrupts are armed.
    auto registerStepGenerator(detail::StepGenerator* generator) noexcept -> void;
}
