#pragma once
#include "hal/drivers/itf/ISpi.hpp"
#include <memory>

namespace hal::spi
{
    // Exclusive ownership per peripheral. STM32 uses the selected CubeMX pin,
    // clock and frame configuration. Unconfigured/owned peripherals return null.
    // Chip select belongs to the caller; creation sends no data.
    [[nodiscard]] auto create(Configuration configuration) -> std::shared_ptr<ISpi>;
}
