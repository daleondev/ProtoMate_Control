#pragma once
#include "hal/drivers/itf/ISpi.hpp"
#include <cstdint>
#include <memory>

namespace hal::spi
{
    enum class Peripheral : std::uint8_t
    {
        Spi1,
        Spi2,
        Spi3,
        Spi4,
        Spi5,
        Spi6
    };

    struct Configuration
    {
        Peripheral peripheral;
    };

    // Exclusive ownership per peripheral. STM32 uses the selected CubeMX pin,
    // clock and frame configuration. Unconfigured/owned peripherals return null.
    // Chip select belongs to the caller; creation sends no data.
    [[nodiscard]] auto create(Configuration configuration) -> std::shared_ptr<ISpi>;
}
