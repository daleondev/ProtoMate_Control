#include "hal/drivers/factory/rgb.hpp"
#include "hal/drivers/impl/linux/RgbOutput.hpp"
#include "hal/drivers/util/ExclusiveInstances.hpp"
namespace hal::rgb {
    auto create(Configuration configuration) -> std::shared_ptr<IRgbOutput> {
        if (configuration.peripheral != Peripheral::Ltdc) { return {}; }
        static util::ExclusiveInstances<1> instances;
        return instances.create<RgbOutput>(0);
    }
}
