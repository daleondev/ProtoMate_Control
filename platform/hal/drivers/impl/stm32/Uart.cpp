#include "hal/drivers/factory/uart.hpp"
#include "hal/drivers/common.hpp"
#include "usart.h"
#include <algorithm>
#include <array>

namespace hal::uart
{
    namespace
    {
        class Uart final : public IUart
        {
          public:
            util::Result<> exchange(std::span<const std::uint8_t> request,
                                    std::span<std::uint8_t> reply,
                                    std::chrono::milliseconds timeout) override
            {
                if (__get_IPSR() != 0 || request.empty() || request.size() > 8 || reply.size() > 8 ||
                    request.size() + reply.size() > 12 || timeout.count() <= 0 || timeout.count() > 1000)
                    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
                // USART2 RX FIFO holds the complete request echo plus reply
                // (at most 12 bytes). No interrupts disabled, no motion DMA used.
                if (huart2.FifoMode != UART_FIFOMODE_ENABLE)
                    return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
                // Replies release the bus four bit times after the stop bit.
                // Let malformed/late traffic finish before flushing RX too.
                HAL_Delay(1);
                __HAL_UART_SEND_REQ(&huart2, UART_RXDATA_FLUSH_REQUEST);
                __HAL_UART_CLEAR_FLAG(&huart2,
                                      UART_CLEAR_OREF | UART_CLEAR_NEF | UART_CLEAR_FEF | UART_CLEAR_PEF);
                const auto start = HAL_GetTick();
                const auto budget = static_cast<std::uint32_t>(timeout.count());
                auto result = HAL_UART_Transmit(&huart2, request.data(), request.size(), budget);
                if (result != HAL_OK)
                    return make_result(result);
                auto remaining = [&] {
                    const auto used = HAL_GetTick() - start;
                    return used < budget ? budget - used : 0U;
                };
                std::array<std::uint8_t, 8> echo{};
                result = HAL_UART_Receive(&huart2, echo.data(), request.size(), remaining());
                if (result != HAL_OK)
                    return make_result(result);
                if (!std::equal(request.begin(), request.end(), echo.begin()))
                    return std::unexpected(std::make_error_code(std::errc::protocol_error));
                if (!reply.empty())
                    result = HAL_UART_Receive(&huart2, reply.data(), reply.size(), remaining());
                if (result == HAL_OK &&
                    (huart2.Instance->ISR & (USART_ISR_ORE | USART_ISR_NE | USART_ISR_FE | USART_ISR_PE)))
                    return std::unexpected(std::make_error_code(std::errc::io_error));
                return make_result(result);
            }
        };
        std::weak_ptr<IUart> owner;
    }
    std::shared_ptr<IUart> createStepperBus()
    {
        if (!owner.expired())
            return {};
        auto bus = std::make_shared<Uart>();
        owner = bus;
        return bus;
    }
}
