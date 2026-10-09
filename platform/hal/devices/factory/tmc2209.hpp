#pragma once

#include "hal/drivers/itf/IUart.hpp"
#include <memory>

namespace hal::device::tmc2209
{
    // Creates a UART transport for TMC2209 communication. STM32 uses the real
    // peripheral; Linux connects a register model owned by the transport.
    // Selection and exclusive ownership follow uart::create(). Sends no traffic.
    [[nodiscard]] auto createTransport(uart::Configuration configuration) -> std::shared_ptr<IUart>;
}
