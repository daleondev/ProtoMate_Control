#pragma once

#include "hal/drivers/itf/IStepGenerator.hpp"

namespace hal::step
{
    // Exclusive TIM2 + DMA1 streams 0..3 + PA0/PB10/PB11 ownership.
    // Releasing the generator AND all its axis views stops/releases the pins.
    [[nodiscard]] auto create() -> std::shared_ptr<IStepGenerator>;
}
