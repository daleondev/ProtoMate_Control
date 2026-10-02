#include "hal/drivers/factory/ethernet.hpp"

#include "hal/drivers/impl/linux/Ethernet.hpp"

#include <memory>

namespace hal::ethernet
{
    auto create(IEthernet::Configuration configuration) -> std::shared_ptr<IEthernet>
    {
        return std::make_shared<Ethernet>(configuration);
    }
}
