#include "hal/drivers/factory/spi.hpp"

namespace hal::spi
{
    std::shared_ptr<ISpi> createEthercatBus() { return {}; }
}
