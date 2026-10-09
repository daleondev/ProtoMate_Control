#include "hal/drivers/factory/spi.hpp"
#include "hal/drivers/impl/linux/Spi.hpp"
#include "hal/drivers/util/ExclusiveInstances.hpp"
#include <utility>

namespace hal::spi
{
    auto create(Configuration configuration) -> std::shared_ptr<ISpi>
    {
        const auto index{ std::to_underlying(configuration.peripheral) };
        if (index >= 6U)
            return {};
        static util::ExclusiveInstances<6> instances;
        return instances.create<Spi>(index, 937'500U);
    }
}
