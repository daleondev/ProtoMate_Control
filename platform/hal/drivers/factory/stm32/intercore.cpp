#include "hal/drivers/factory/intercore.hpp"
#include "hal/drivers/impl/stm32/InterCoreChannel.hpp"

#include <mutex>

namespace hal::intercore
{
    auto create(util::Core owner) -> std::shared_ptr<IInterCoreChannel>
    {
        if (owner != util::executing_core()) { return {}; }
        static std::mutex mutex;
        static std::weak_ptr<IInterCoreChannel> endpoint;
        const std::scoped_lock lock(mutex);
        if (!endpoint.expired()) { return {}; }
        auto result = std::make_shared<InterCoreChannel>();
        endpoint = result;
        return result;
    }
}
