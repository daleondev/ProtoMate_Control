#include "hal/drivers/factory/spi.hpp"
#include "hal/drivers/impl/linux/Spi.hpp"
#include "hal/linux/Mutex.hpp"
#include <array>
#include <mutex>
#include <utility>

namespace hal::spi
{
    auto create(Configuration configuration) -> std::shared_ptr<ISpi>
    {
        const auto index{ std::to_underlying(configuration.peripheral) };
        if (index >= 6U)
            return {};
        static linux::Mutex mutex;
        static std::array<std::weak_ptr<ISpi>, 6> owners;
        const std::scoped_lock lock{ mutex };
        auto& owner{ owners[index] };
        if (!owner.expired())
            return {};
        auto bus{ std::make_shared<Spi>(937'500U) };
        owner = bus;
        return bus;
    }
}
