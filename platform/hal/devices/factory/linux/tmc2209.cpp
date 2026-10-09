#include "hal/devices/factory/tmc2209.hpp"
#include "hal/drivers/factory/uart.hpp"
#include "hal/devices/impl/linux/Tmc2209Model.hpp"
#include "hal/drivers/impl/linux/Uart.hpp"

namespace hal::device::tmc2209
{
    auto createTransport(uart::Configuration configuration) -> std::shared_ptr<IUart>
    {
        auto transport{ uart::create(configuration) };
        if (transport) {
            std::static_pointer_cast<Uart>(transport)->setExchangeHandler(
                [model = std::make_shared<Tmc2209Model>()](auto tx, auto rx, auto timeout) {
                    return model->exchange(tx, rx, timeout);
                });
        }
        return transport;
    }
}
