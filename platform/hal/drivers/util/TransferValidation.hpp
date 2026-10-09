#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace hal::util
{
    // STM32 polling APIs use 16-bit byte counts and a 32-bit millisecond
    // budget. The all-ones timeout is reserved for an unbounded HAL wait.
    constexpr auto validTransferSize(std::size_t size) noexcept -> bool
    {
        return size <= std::numeric_limits<std::uint16_t>::max();
    }
    constexpr auto validTransferTimeout(std::chrono::milliseconds timeout) noexcept -> bool
    {
        return timeout.count() > 0 && timeout.count() < std::numeric_limits<std::uint32_t>::max();
    }
}
