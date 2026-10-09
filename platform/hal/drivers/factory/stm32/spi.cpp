#include "hal/drivers/factory/spi.hpp"
#include "hal/drivers/impl/stm32/Spi.hpp"
#include "spi.h"

#include <array>
#include <mutex>
#include <optional>
#include <utility>

namespace hal::spi
{
    namespace
    {
        auto hardware(Peripheral peripheral) -> std::optional<Spi::Configuration>
        {
            switch (peripheral) {
#define HAL_SPI_INSTANCE(number)                                                                             \
    case Peripheral::Spi##number:                                                                            \
        return Spi::Configuration{                                                                           \
            .handle = hspi##number,                                                                          \
            .kernel_clock_hz = [] { return HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI##number); },         \
            .initialize = MX_SPI##number##_Init,                                                             \
            .recover =                                                                                       \
              [] {                                                                                           \
            __HAL_RCC_SPI##number##_FORCE_RESET();                                                           \
            __HAL_RCC_SPI##number##_RELEASE_RESET();                                                         \
            MX_SPI##number##_Init();                                                                         \
        },                                                                                                   \
        };
#include "spi_instances.inc"
#undef HAL_SPI_INSTANCE
                default:
                    return {};
            }
        }
    }

    auto create(Configuration configuration) -> std::shared_ptr<ISpi>
    {
        const auto selected{ hardware(configuration.peripheral) };
        if (!selected)
            return {};
        static std::mutex mutex;
        static std::array<std::weak_ptr<ISpi>, 6> owners;
        const std::scoped_lock lock{ mutex };
        auto& owner{ owners[std::to_underlying(configuration.peripheral)] };
        if (!owner.expired())
            return {};
        auto bus{ std::make_shared<Spi>(*selected) };
        owner = bus;
        return bus;
    }
}
