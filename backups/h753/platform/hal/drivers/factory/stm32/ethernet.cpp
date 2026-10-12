#include "hal/drivers/factory/ethernet.hpp"

#include "hal/drivers/impl/stm32/Ethernet.hpp"
#include "hal/hal.hpp"

#include <memory>

namespace
{
    inline constexpr auto MAX_PHY_ADDRESS{ 31U };
}

namespace hal::ethernet
{
    auto create(IEthernet::Configuration configuration) -> std::shared_ptr<IEthernet>
    {
        if (configuration.phy_address > MAX_PHY_ADDRESS) {
            return {};
        }
        static auto driver{ std::make_shared<Ethernet>(
          Ethernet::HardwareConfiguration{ .handle = heth, .configuration = configuration }) };
        // There is one MAC/DMA engine. Do not silently return a differently
        // configured existing instance (in particular, with another RX filter).
        return driver->matchesConfiguration(configuration) ? driver : nullptr;
    }
}
