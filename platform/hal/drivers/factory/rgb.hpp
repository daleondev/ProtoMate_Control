#pragma once
#include "hal/drivers/itf/IRgbOutput.hpp"
#include "hal/drivers/itf/GpioTypes.hpp"
#include <memory>
namespace hal::rgb {
    enum class Peripheral { Ltdc };
    struct Configuration { Peripheral peripheral{Peripheral::Ltdc}; gpio::Pin backlight{}; };
    auto create(Configuration configuration) -> std::shared_ptr<IRgbOutput>;
}
