#pragma once
#include "hal/drivers/itf/IGraphics.hpp"
#include <cstdint>
#include <memory>
namespace hal::graphics
{
    enum class Peripheral : std::uint8_t
    {
        Dma2d
    };
    auto create(Peripheral peripheral) -> std::shared_ptr<IGraphics>;
}
