#pragma once

#include "hal/drivers/itf/IGpio.hpp"
#include "hal/drivers/itf/IQuadratureEncoder.hpp"

#include <cstdint>
#include <memory>

namespace hal::encoder
{
    struct Configuration
    {
        std::uint8_t timer;
        gpio::Pin a;
        gpio::Pin b;
    };

    // Supported route: TIM3_CH1/PB4 + TIM3_CH2/PB5. Claims timer and both pins.
    // Z/index is a separate IDigitalInput; it never implicitly resets position.
    [[nodiscard]] auto create(Configuration configuration) -> std::shared_ptr<IQuadratureEncoder>;
}
