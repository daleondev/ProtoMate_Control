#pragma once

#include "hal/drivers/itf/ITimer.hpp"
#include <cstdint>
#include <memory>

namespace hal::timer
{
    enum class Peripheral : std::uint8_t
    {
        Tim1 = 1,
        Tim2,
        Tim3,
        Tim4,
        Tim5,
        Tim6,
        Tim7,
        Tim8,
        Tim12 = 12,
        Tim13,
        Tim14,
        Tim15,
        Tim16,
        Tim17
    };
    enum class Channel : std::uint8_t
    {
        Channel1 = 1,
        Channel2,
        Channel3,
        Channel4
    };
    struct Configuration
    {
        Peripheral peripheral;
    };

    // TIM5 is the shared runtime timer. Other timers are reserved for the HAL
    // tick, step generator or the exclusive PWM/encoder factories.
    [[nodiscard]] auto create(Configuration configuration) -> std::shared_ptr<ITimer>;
}
