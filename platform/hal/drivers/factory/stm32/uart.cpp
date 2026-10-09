#include "hal/drivers/factory/uart.hpp"
#include "hal/drivers/impl/stm32/Uart.hpp"
#include "hal/drivers/util/ExclusiveInstances.hpp"
#include "hal/drivers/util/UartTransfer.hpp"
#include "usart.h"

#include <optional>
#include <utility>

namespace hal::uart
{
    namespace
    {
        auto hardware(Configuration configuration) -> std::optional<Uart::HardwareConfiguration>
        {
            switch (configuration.peripheral) {
#define HAL_UART_INSTANCE(id, handle_name, initialize_function)                                              \
    case Peripheral::id:                                                                                     \
        return Uart::HardwareConfiguration{                                                                  \
            .handle = handle_name,                                                                           \
            .configuration = configuration.transport,                                                        \
            .initialize = initialize_function,                                                               \
        };
#include "uart_instances.inc"
#undef HAL_UART_INSTANCE
                default:
                    return {};
            }
        }
    }

    auto create(Configuration configuration) -> std::shared_ptr<IUart>
    {
        // The BSP owns USART3 independently of CubeMX's UART declarations.
        if (configuration.peripheral == Peripheral::Usart3 ||
            !util::validUartConfiguration(configuration.transport))
            return {};
        const auto selected{ hardware(configuration) };
        if (!selected)
            return {};
        static util::ExclusiveInstances<9> instances;
        return instances.create<Uart>(std::to_underlying(configuration.peripheral), *selected);
    }
}
