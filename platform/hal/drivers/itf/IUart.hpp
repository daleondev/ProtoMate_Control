#pragma once

#include "hal/util/Result.hpp"
#include <chrono>
#include <cstdint>
#include <span>

namespace hal
{
    // Exclusive, thread-context request/reply transport. The owner serializes
    // the complete bus, including multi-transaction register operations.
    class IUart
    {
      public:
        struct Configuration
        {
            bool local_echo{};
            std::chrono::milliseconds settle_time{};
        };

        virtual ~IUart() = default;
        // Transmit the request, optionally consume/validate its local echo,
        // then receive exactly reply.size() bytes. Empty reply is write-only.
        // Discards stale input after settle_time; timeout covers the exchange.
        virtual util::Result<> exchange(std::span<const std::uint8_t> request,
                                        std::span<std::uint8_t> reply,
                                        std::chrono::milliseconds timeout) = 0;
    };
}

namespace hal::uart
{
    enum class Peripheral : std::uint8_t
    {
        Usart1,
        Usart2,
        Usart3,
        Uart4,
        Uart5,
        Usart6,
        Uart7,
        Uart8,
        Lpuart1
    };

    struct Configuration
    {
        Peripheral peripheral;
        IUart::Configuration transport{};
    };
}
