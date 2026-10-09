#include "Spi.hpp"
#include "hal/drivers/detail/TransferValidation.hpp"
#include <utility>

namespace hal
{
    Spi::Spi(std::uint32_t clock_frequency_hz)
      : m_clockFrequencyHz{ clock_frequency_hz }
    {
    }

    auto Spi::exchange(std::span<const std::uint8_t> tx,
                       std::span<std::uint8_t> rx,
                       std::chrono::milliseconds timeout) -> util::Result<>
    {
        if (tx.empty() || tx.size() != rx.size() || !detail::validTransferSize(tx.size()) ||
            !detail::validTransferTimeout(timeout))
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        if (!m_handler)
            return std::unexpected(std::make_error_code(std::errc::not_connected));
        return m_handler(tx, rx, timeout);
    }

    auto Spi::clockFrequencyHz() const noexcept -> std::uint32_t { return m_clockFrequencyHz; }
    auto Spi::setExchangeHandler(ExchangeHandler handler) -> void { m_handler = std::move(handler); }
}
