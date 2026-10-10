#include "hal/drivers/factory/uart.hpp"
#include "hal/drivers/impl/linux/Uart.hpp"
#include "hal/drivers/util/ExclusiveInstances.hpp"
#include "hal/drivers/util/UartTransfer.hpp"
#include <utility>

namespace
{
    inline constexpr auto UART_INSTANCE_COUNT{ 9U };
    inline constexpr auto UART_POOL_SIZE{ 9 };
}

namespace hal::uart
{
    auto create(Configuration configuration) -> std::shared_ptr<IUart>
    {
        const auto index{ std::to_underlying(configuration.peripheral) };
        if (configuration.peripheral == Peripheral::Usart3 ||
            !util::valid_uart_configuration(configuration.transport) || index >= UART_INSTANCE_COUNT)
            return {};
        static util::ExclusiveInstances<UART_POOL_SIZE> instances;
        return instances.create<Uart>(index, configuration.transport);
    }
}
