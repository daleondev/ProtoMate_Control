#pragma once
#include "hal/drivers/itf/IGraphics.hpp"
#include <memory>
namespace hal::graphics
{
    enum class Peripheral
    {
        Dma2d
    };
    auto create(Peripheral peripheral) -> std::shared_ptr<IGraphics>;
}
