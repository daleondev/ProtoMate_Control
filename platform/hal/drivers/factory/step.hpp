#pragma once

#include "hal/drivers/itf/IStepGenerator.hpp"
#include "hal/drivers/itf/IDigitalInput.hpp"
#include <array>

namespace hal::step
{
    struct Configuration { std::array<gpio::Pin, 3> pins{}; };

    // Exclusive TIM2 CH1/3/4 + DMA1 streams 0..3 and the selected pins.
    // Releasing the generator AND all its axis views stops/releases the pins.
    [[nodiscard]] auto create(Configuration configuration = {}) -> std::shared_ptr<IStepGenerator>;
}
