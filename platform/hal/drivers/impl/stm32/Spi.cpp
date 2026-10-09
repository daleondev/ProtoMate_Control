#include "Spi.hpp"
#include "hal/drivers/common.hpp"
#include "hal/drivers/detail/TransferValidation.hpp"

namespace hal
{
    Spi::Spi(Configuration configuration)
      : m_configuration{ configuration }
    {
        m_configuration.initialize();
    }

    Spi::~Spi() { static_cast<void>(HAL_SPI_DeInit(&m_configuration.handle)); }

    auto Spi::clockFrequencyHz() const noexcept -> std::uint32_t
    {
        const auto prescaler{ m_configuration.handle.Init.BaudRatePrescaler >> SPI_CFG1_MBR_Pos };
        return m_configuration.kernel_clock_hz() / (2U << prescaler);
    }

    auto Spi::exchange(std::span<const std::uint8_t> tx,
                       std::span<std::uint8_t> rx,
                       std::chrono::milliseconds timeout) -> util::Result<>
    {
        if (__get_IPSR() != 0 || tx.empty() || tx.size() != rx.size() ||
            !detail::validTransferSize(tx.size()) || !detail::validTransferTimeout(timeout))
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));

        // This interface transfers bytes in full duplex. A wider CubeMX frame
        // would make the HAL read/write beyond the supplied byte buffers.
        const auto& settings{ m_configuration.handle.Init };
        if (settings.DataSize != SPI_DATASIZE_8BIT || settings.Direction != SPI_DIRECTION_2LINES)
            return std::unexpected(std::make_error_code(std::errc::operation_not_supported));

        const auto status{ HAL_SPI_TransmitReceive(&m_configuration.handle,
                                                   tx.data(),
                                                   rx.data(),
                                                   static_cast<std::uint16_t>(tx.size()),
                                                   static_cast<std::uint32_t>(timeout.count())) };
        if (status != HAL_OK) {
            // Discard a partial FIFO transaction before the next exchange.
            // Chip select is independently owned and released by the caller.
            m_configuration.recover();
        }
        return make_result(status);
    }
}
