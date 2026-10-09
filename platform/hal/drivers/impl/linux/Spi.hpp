#pragma once

#include "hal/drivers/itf/ISpi.hpp"
#include <functional>

namespace hal
{
    // In-process transport model. Device factories/tests attach peer behavior.
    class Spi final : public ISpi
    {
      public:
        using ExchangeHandler = std::function<
          util::Result<>(std::span<const std::uint8_t>, std::span<std::uint8_t>, std::chrono::milliseconds)>;
        explicit Spi(std::uint32_t clock_frequency_hz);

        Spi(const Spi&) = delete;
        Spi& operator=(const Spi&) = delete;
        Spi(Spi&&) = delete;
        Spi& operator=(Spi&&) = delete;

        [[nodiscard]] auto exchange(std::span<const std::uint8_t> tx,
                                    std::span<std::uint8_t> rx,
                                    std::chrono::milliseconds timeout) -> util::Result<> override;
        [[nodiscard]] auto clockFrequencyHz() const noexcept -> std::uint32_t override;
        // Configure while idle, before sharing the transport with its owner.
        auto setExchangeHandler(ExchangeHandler handler) -> void;

      private:
        std::uint32_t m_clockFrequencyHz;
        ExchangeHandler m_handler;
    };
}
