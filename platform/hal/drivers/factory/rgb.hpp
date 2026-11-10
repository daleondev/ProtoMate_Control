#pragma once
#include "hal/drivers/itf/GpioTypes.hpp"
#include "hal/drivers/itf/IRgbOutput.hpp"
#include <cstdint>
#include <memory>
namespace hal::rgb
{
    enum class Peripheral : std::uint8_t
    {
        Ltdc
    };
    struct Configuration
    {
        Peripheral peripheral{ Peripheral::Ltdc };
        gpio::Pin backlight{};
    };
    auto create(Configuration configuration) -> std::shared_ptr<IRgbOutput>;
}
