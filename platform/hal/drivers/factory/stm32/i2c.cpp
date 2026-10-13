#include "hal/drivers/factory/i2c.hpp"
#if defined(CORE_CM7)
#include "hal/drivers/impl/stm32/I2c.hpp"
#include "hal/drivers/util/ExclusiveInstances.hpp"
#include "i2c.h"
#endif
namespace hal::i2c {
    auto create(Peripheral peripheral) -> std::shared_ptr<II2c> {
#if defined(CORE_CM7)
        if (peripheral != Peripheral::I2c4) { return {}; }
        static util::ExclusiveInstances<1> instances;
        return instances.create<I2c>(0, hi2c4);
#else
        static_cast<void>(peripheral); return {};
#endif
    }
}
