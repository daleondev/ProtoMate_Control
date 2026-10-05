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
        virtual ~IUart() = default;
        // TX is coupled to RX through a resistor. Consume/validate local echo,
        // then receive exactly reply.size() bytes. Empty reply means write-only.
        virtual util::Result<> exchange(std::span<const std::uint8_t> request,
                                        std::span<std::uint8_t> reply,
                                        std::chrono::milliseconds timeout) = 0;
    };
}
