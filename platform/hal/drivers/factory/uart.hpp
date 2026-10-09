#pragma once
#include "hal/drivers/itf/IUart.hpp"
#include <cstdint>
#include <memory>

namespace hal::uart
{
    enum class Peripheral : std::uint8_t
    {
        Usart1,
        Usart2,
        Usart3,
        Uart4,
        Uart5,
        Usart6,
        Uart7,
        Uart8,
        Lpuart1
    };

    struct Configuration
    {
        Peripheral peripheral;
        IUart::Configuration transport{};
    };

    // Exclusive ownership per configured peripheral. STM32 uses its CubeMX pin
    // and baud configuration. USART3 is reserved for the console.
    // Invalid/unconfigured/owned peripherals return null; creation sends no data.
    [[nodiscard]] auto create(Configuration configuration) -> std::shared_ptr<IUart>;
}
