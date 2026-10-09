#pragma once

#include "hal/drivers/detail/TransferValidation.hpp"
#include "hal/drivers/itf/IUart.hpp"

namespace hal::detail
{
    // Polling transmit completes before receive starts. When local echo is
    // enabled the RX FIFO must fit both the echo and an immediate reply.
    inline constexpr std::size_t uart_fifo_bytes{ 16U };

    constexpr auto validUartConfiguration(IUart::Configuration configuration) noexcept -> bool
    {
        return configuration.settle_time.count() >= 0 &&
               configuration.settle_time.count() < std::numeric_limits<std::uint32_t>::max();
    }

    constexpr auto validUartTransfer(IUart::Configuration configuration,
                                     std::size_t request_size,
                                     std::size_t reply_size,
                                     std::chrono::milliseconds timeout) noexcept -> bool
    {
        return validUartConfiguration(configuration) && request_size > 0 && validTransferSize(request_size) &&
               validTransferSize(reply_size) && validTransferTimeout(timeout) &&
               configuration.settle_time < timeout &&
               (!configuration.local_echo || request_size + reply_size <= uart_fifo_bytes);
    }
}
