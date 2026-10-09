#pragma once

#include "hal/utilities/Result.hpp"
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
