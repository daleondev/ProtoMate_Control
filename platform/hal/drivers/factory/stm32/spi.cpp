#include "hal/drivers/factory/spi.hpp"
#include "hal/drivers/impl/stm32/Spi.hpp"
#include "hal/drivers/util/ExclusiveInstances.hpp"
#if defined(CORE_CM4)
#include "spi.h"
#endif
namespace hal::spi {
    auto create(Configuration configuration) -> std::shared_ptr<ISpi>
    {
#if defined(CORE_CM4)
        if (configuration.peripheral != Peripheral::Spi5 || hspi5.State != HAL_SPI_STATE_READY) { return {}; }
        static util::ExclusiveInstances<1> instances;
        return instances.create<Spi>(0, Spi::Configuration{
            .handle = hspi5,
            .kernel_clock_hz = [] { return HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI5); },
            .recover = [] { static_cast<void>(HAL_SPI_Abort(&hspi5)); }
        });
#else
        static_cast<void>(configuration);
        return {};
#endif
    }
}
