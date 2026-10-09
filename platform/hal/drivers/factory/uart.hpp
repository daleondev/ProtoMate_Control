#pragma once
#include "hal/drivers/itf/IUart.hpp"
#include <memory>

namespace hal::uart
{
    // Exclusive ownership per configured peripheral. STM32 uses its CubeMX pin
    // and baud configuration. USART3 is reserved for the console.
    // Invalid/unconfigured/owned peripherals return null; creation sends no data.
    [[nodiscard]] auto create(Configuration configuration) -> std::shared_ptr<IUart>;
}
