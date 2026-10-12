#include "hal/drivers/factory/uart.hpp"
#include "hal/drivers/impl/stm32/Uart.hpp"
#include "hal/drivers/util/ExclusiveInstances.hpp"
#include "hal/drivers/util/UartTransfer.hpp"
#include "usart.h"

#include <optional>
#include <utility>

namespace
{
    inline constexpr auto UART_POOL_SIZE{ 9 };
}

namespace hal::uart
{
    namespace
    {
        auto hardware(Configuration configuration) -> std::optional<Uart::HardwareConfiguration>
        {
            switch (configuration.peripheral) {
// Generated X-macro entries supply declaration identifiers, not expressions.
// NOLINTBEGIN(cppcoreguidelines-macro-usage,bugprone-macro-parentheses)
#define HAL_UART_INSTANCE(id, handle_name, initialize_function)                                              \
    case Peripheral::id:                                                                                     \
        return Uart::HardwareConfiguration{                                                                  \
            .handle = handle_name,                                                                           \
            .configuration = configuration.transport,                                                        \
            .initialize = initialize_function,                                                               \
        };
#include "uart_instances.inc"
#undef HAL_UART_INSTANCE
                    // NOLINTEND(cppcoreguidelines-macro-usage,bugprone-macro-parentheses)
                default:
                    return {};
            }
        }
    }

    auto create(Configuration configuration) -> std::shared_ptr<IUart>
    {
        // The BSP owns USART3 independently of CubeMX's UART declarations.
        if (configuration.peripheral == Peripheral::Usart3 ||
            !util::valid_uart_configuration(configuration.transport)) {
            return {};
        }
        const auto selected{ hardware(configuration) };
        if (!selected) {
            return {};
        }
        static util::ExclusiveInstances<UART_POOL_SIZE> instances;
        return instances.create<Uart>(std::to_underlying(configuration.peripheral), *selected);
    }
}
