#include "hal/devices/factory/tmc2209.hpp"

namespace hal::device::tmc2209
{
    auto createTransport(uart::Configuration configuration) -> std::shared_ptr<IUart>
    {
        return uart::create(configuration);
    }
}
