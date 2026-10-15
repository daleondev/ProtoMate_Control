#include "hal/drivers/factory/graphics.hpp"
#if defined(CORE_CM7)
#include "hal/drivers/impl/stm32/Graphics.hpp"
#include "hal/drivers/util/ExclusiveInstances.hpp"
#endif
namespace hal::graphics
{
    auto create(Peripheral peripheral) -> std::shared_ptr<IGraphics>
    {
#if defined(CORE_CM7)
        if (peripheral != Peripheral::Dma2d) {
            return {};
        }
        static util::ExclusiveInstances<1> instances;
        return instances.create<Graphics>(0, *DMA2D);
#else
        static_cast<void>(peripheral);
        return {};
#endif
    }
}
