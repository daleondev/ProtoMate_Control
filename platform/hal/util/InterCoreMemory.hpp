#pragma once

#include "hal/drivers/itf/IInterCoreChannel.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <type_traits>

namespace hal::util
{
    // Shared ABI: fixed-width fields and bytes only. Each queue is SPSC.
    // Publication of write/read counters transfers ownership of a frame slot.
    // On STM32 the entire region is Normal, shareable, non-cacheable memory.
    struct InterCoreQueue
    {
        static constexpr std::uint32_t CAPACITY{4};
        static constexpr std::size_t MAX_FRAME_BYTES{2048};
        struct Frame
        {
            std::uint32_t size;
            std::array<std::byte, MAX_FRAME_BYTES> bytes;
        };
        alignas(4) std::uint32_t written;
        alignas(4) std::uint32_t read;
        std::array<Frame, CAPACITY> frames;

        std::expected<void, IInterCoreChannel::Error> push(std::span<const std::byte> bytes) noexcept
        {
            using Error = IInterCoreChannel::Error;
            if (bytes.size() > MAX_FRAME_BYTES) { return std::unexpected(Error::TooLarge); }
            const auto tail = std::atomic_ref(written).load(std::memory_order_relaxed);
            const auto head = std::atomic_ref(read).load(std::memory_order_acquire);
            if (tail - head >= CAPACITY) { return std::unexpected(Error::WouldBlock); }
            auto& slot = frames[tail % CAPACITY];
            std::ranges::copy(bytes, slot.bytes.begin());
            slot.size = static_cast<std::uint32_t>(bytes.size());
            std::atomic_ref(written).store(tail + 1U, std::memory_order_release);
            return {};
        }

        std::expected<std::size_t, IInterCoreChannel::Error> pop(std::span<std::byte> buffer) noexcept
        {
            using Error = IInterCoreChannel::Error;
            const auto head = std::atomic_ref(read).load(std::memory_order_relaxed);
            const auto tail = std::atomic_ref(written).load(std::memory_order_acquire);
            if (tail == head) { return std::unexpected(Error::WouldBlock); }
            const auto& slot = frames[head % CAPACITY];
            if (tail - head > CAPACITY || slot.size > MAX_FRAME_BYTES) {
                return std::unexpected(Error::Disconnected);
            }
            if (slot.size > buffer.size()) { return std::unexpected(Error::BufferTooSmall); }
            const auto size = slot.size;
            std::ranges::copy(std::span(slot.bytes).first(size), buffer.begin());
            std::atomic_ref(read).store(head + 1U, std::memory_order_release);
            return size;
        }
    };

    struct InterCoreMemory
    {
        static constexpr std::uint32_t MAGIC{0x50325031}; // P2P1
        static constexpr std::uint32_t VERSION{1};
        std::uint32_t magic;
        std::uint32_t version;
        // 0 = not opened, 1 = open, 2 = closed. Only the named side writes it.
        std::array<std::uint32_t, 2> state;
        // [destination: M4=0, M7=1][normal=0, urgent=1]
        std::array<std::array<InterCoreQueue, 2>, 2> inbox;
    };
    static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic_ref<std::uint32_t>::required_alignment <= 4);
    static_assert(std::is_trivially_default_constructible_v<InterCoreMemory> && std::is_trivially_copyable_v<InterCoreMemory> && std::is_standard_layout_v<InterCoreMemory>);
    static_assert(sizeof(InterCoreMemory) == 32880);
}
