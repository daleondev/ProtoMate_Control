#include "hal/drivers/factory/graphics.hpp"
#include "hal/drivers/impl/linux/Graphics.hpp"
#include "hal/drivers/util/ExclusiveInstances.hpp"
namespace hal::graphics
{
    auto create(Peripheral peripheral) -> std::shared_ptr<IGraphics>
    {
        if (peripheral != Peripheral::Dma2d) {
            return {};
        }
        static util::ExclusiveInstances<1> instances;
        return instances.create<Graphics>(0);
    }
}
