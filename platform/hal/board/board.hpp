#pragma once

#include "hal/devices/itf/IButton.hpp"
#include "hal/devices/itf/ILed.hpp"

#include <cstdint>
#include <memory>

namespace hal::board
{
    enum class LedId : std::uint8_t
    {
        Green,
        Yellow,
        Red
    };

    enum class ButtonId : std::uint8_t
    {
        User
    };

    [[nodiscard]] auto createLed(LedId id) -> std::shared_ptr<device::ILed>;
    [[nodiscard]] auto createButton(ButtonId id) -> std::shared_ptr<device::IButton>;
}
