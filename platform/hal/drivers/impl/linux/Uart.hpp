#pragma once

#include "hal/drivers/itf/IUart.hpp"
#include <functional>

namespace hal
{
    // In-process transport model; protocol simulation belongs to devices.
    class Uart final : public IUart
    {
      public:
        using ExchangeHandler = std::function<
          util::Result<>(std::span<const std::uint8_t>, std::span<std::uint8_t>, std::chrono::milliseconds)>;
        explicit Uart(Configuration configuration);

        Uart(const Uart&) = delete;
        Uart& operator=(const Uart&) = delete;
        Uart(Uart&&) = delete;
        Uart& operator=(Uart&&) = delete;

        [[nodiscard]] auto exchange(std::span<const std::uint8_t> request,
                                    std::span<std::uint8_t> reply,
                                    std::chrono::milliseconds timeout) -> util::Result<> override;
        // Handler models the peer after wire-level echo removal. Configure while
        // idle, before sharing the transport with its owner.
        auto setExchangeHandler(ExchangeHandler handler) -> void;

      private:
        Configuration m_configuration;
        ExchangeHandler m_handler;
    };
}
