#pragma once

#include "hal/drivers/itf/IQuadratureEncoder.hpp"

#include <memory>

namespace hal::encoder
{
    // Supported route: TIM3_CH1/PB4 + TIM3_CH2/PB5. Claims timer and both pins.
    // Z/index is a separate IDigitalInput; it never implicitly resets position.
    [[nodiscard]] auto create(Configuration configuration) -> std::shared_ptr<IQuadratureEncoder>;
}
