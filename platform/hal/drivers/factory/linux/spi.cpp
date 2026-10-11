#include "hal/drivers/factory/spi.hpp"
#include "hal/drivers/impl/linux/Spi.hpp"
#include "hal/drivers/util/ExclusiveInstances.hpp"
#include <utility>

namespace
{
    inline constexpr auto SPI_INSTANCE_COUNT{ 6U };
    inline constexpr auto SPI_POOL_SIZE{ 6 };
    inline constexpr auto SIMULATED_SPI_CLOCK_HZ{ 937'500U };
}

namespace hal::spi
{
    auto create(Configuration configuration) -> std::shared_ptr<ISpi>
    {
        const auto index{ std::to_underlying(configuration.peripheral) };
        if (index >= SPI_INSTANCE_COUNT) {
            return {};
        }
        static util::ExclusiveInstances<SPI_POOL_SIZE> instances;
        return instances.create<Spi>(index, SIMULATED_SPI_CLOCK_HZ);
    }
}
