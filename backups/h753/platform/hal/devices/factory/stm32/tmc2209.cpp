#include "hal/devices/factory/tmc2209.hpp"
#include "hal/drivers/factory/uart.hpp"

namespace hal::device::tmc2209
{
    auto create_transport(uart::Configuration configuration) -> std::shared_ptr<IUart>
    {
        return uart::create(configuration);
    }
}
