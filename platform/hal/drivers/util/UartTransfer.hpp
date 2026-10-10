#pragma once

#include "hal/drivers/itf/IUart.hpp"
#include "hal/drivers/util/TransferValidation.hpp"

namespace hal::util
{
    // Polling transmit completes before receive starts. When local echo is
    // enabled the RX FIFO must fit both the echo and an immediate reply.
    inline constexpr std::size_t UART_FIFO_BYTES{ 16U };

    constexpr auto valid_uart_configuration(IUart::Configuration configuration) noexcept -> bool
    {
        return configuration.settle_time.count() >= 0 &&
               configuration.settle_time.count() < std::numeric_limits<std::uint32_t>::max();
    }

    constexpr auto valid_uart_transfer(IUart::Configuration configuration,
                                       std::size_t request_size,
                                       std::size_t reply_size,
                                       std::chrono::milliseconds timeout) noexcept -> bool
    {
        return valid_uart_configuration(configuration) && request_size > 0 &&
               valid_transfer_size(request_size) && valid_transfer_size(reply_size) &&
               valid_transfer_timeout(timeout) && configuration.settle_time < timeout &&
               (!configuration.local_echo || request_size + reply_size <= UART_FIFO_BYTES);
    }
}
