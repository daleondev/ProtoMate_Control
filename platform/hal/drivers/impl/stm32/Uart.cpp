#include "Uart.hpp"
#include "hal/stm32/HalResult.hpp"
#include "hal/drivers/util/UartTransfer.hpp"

#include <algorithm>
#include <array>

namespace hal
{
    Uart::Uart(HardwareConfiguration configuration)
      : m_handle{ configuration.handle }
      , m_configuration{ configuration.configuration }
    {
        configuration.initialize();
    }
    Uart::~Uart() { static_cast<void>(HAL_UART_DeInit(&m_handle)); }

    auto Uart::exchange(std::span<const std::uint8_t> request,
                        std::span<std::uint8_t> reply,
                        std::chrono::milliseconds timeout) -> util::Result<>
    {
        if (__get_IPSR() != 0 ||
            !util::validUartTransfer(m_configuration, request.size(), reply.size(), timeout))
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        if (m_configuration.local_echo && m_handle.FifoMode != UART_FIFOMODE_ENABLE)
            return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
        if (m_handle.Init.WordLength == UART_WORDLENGTH_9B && m_handle.Init.Parity == UART_PARITY_NONE)
            return std::unexpected(std::make_error_code(std::errc::operation_not_supported));

        const auto start{ HAL_GetTick() };
        const auto budget{ static_cast<std::uint32_t>(timeout.count()) };
        const auto remaining = [&] {
            const auto used{ HAL_GetTick() - start };
            return used < budget ? budget - used : 0U;
        };
        if (m_configuration.settle_time.count() > 0)
            HAL_Delay(static_cast<std::uint32_t>(m_configuration.settle_time.count()));
        __HAL_UART_SEND_REQ(&m_handle, UART_RXDATA_FLUSH_REQUEST);
        __HAL_UART_CLEAR_FLAG(&m_handle, UART_CLEAR_OREF | UART_CLEAR_NEF | UART_CLEAR_FEF | UART_CLEAR_PEF);
        if (!remaining())
            return std::unexpected(std::make_error_code(std::errc::timed_out));
        auto result{ HAL_UART_Transmit(
          &m_handle, request.data(), static_cast<std::uint16_t>(request.size()), remaining()) };
        if (result != HAL_OK)
            return make_result(result);
        if (m_configuration.local_echo) {
            std::array<std::uint8_t, util::uart_fifo_bytes> echo{};
            result = HAL_UART_Receive(
              &m_handle, echo.data(), static_cast<std::uint16_t>(request.size()), remaining());
            if (result != HAL_OK)
                return make_result(result);
            if (!std::equal(request.begin(), request.end(), echo.begin()))
                return std::unexpected(std::make_error_code(std::errc::protocol_error));
        }
        if (!reply.empty())
            result = HAL_UART_Receive(
              &m_handle, reply.data(), static_cast<std::uint16_t>(reply.size()), remaining());
        if (result == HAL_OK &&
            (m_handle.Instance->ISR & (USART_ISR_ORE | USART_ISR_NE | USART_ISR_FE | USART_ISR_PE)))
            return std::unexpected(std::make_error_code(std::errc::io_error));
        return make_result(result);
    }
}
