#pragma once

#include "hal/utilities/Result.hpp"
#include <chrono>
#include <cstdint>
#include <span>

namespace hal
{
    // Exclusive synchronous bus, used from thread context. The owner serializes
    // transactions and controls chip select around the complete exchange.
    class ISpi
    {
      public:
        virtual ~ISpi() = default;
        // Equal, nonempty buffers; one received byte per transmitted byte.
        // No DMA: ordinary stack buffers are valid, including with D-cache on.
        virtual util::Result<> exchange(std::span<const std::uint8_t> tx,
                                        std::span<std::uint8_t> rx,
                                        std::chrono::milliseconds timeout) = 0;
        [[nodiscard]] virtual std::uint32_t clockFrequencyHz() const noexcept = 0;
    };
}
