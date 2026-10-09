#include "Uart.hpp"
#include "hal/drivers/detail/UartTransfer.hpp"
#include <utility>

namespace hal
{
    Uart::Uart(Configuration configuration)
      : m_configuration{ configuration }
    {
    }

    auto Uart::exchange(std::span<const std::uint8_t> request,
                        std::span<std::uint8_t> reply,
                        std::chrono::milliseconds timeout) -> util::Result<>
    {
        if (!detail::validUartTransfer(m_configuration, request.size(), reply.size(), timeout))
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        if (!m_handler)
            return std::unexpected(std::make_error_code(std::errc::not_connected));
        return m_handler(request, reply, timeout - m_configuration.settle_time);
    }

    auto Uart::setExchangeHandler(ExchangeHandler handler) -> void { m_handler = std::move(handler); }
}
