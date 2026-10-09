#include "hal/drivers/factory/uart.hpp"
#include "hal/drivers/impl/linux/Uart.hpp"
#include "hal/drivers/util/ExclusiveInstances.hpp"
#include "hal/drivers/util/UartTransfer.hpp"
#include <utility>

namespace hal::uart
{
    auto create(Configuration configuration) -> std::shared_ptr<IUart>
    {
        const auto index{ std::to_underlying(configuration.peripheral) };
        if (configuration.peripheral == Peripheral::Usart3 ||
            !util::validUartConfiguration(configuration.transport) || index >= 9U)
            return {};
        static util::ExclusiveInstances<9> instances;
        return instances.create<Uart>(index, configuration.transport);
    }
}
