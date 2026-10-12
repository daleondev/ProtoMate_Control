#pragma once

#include "communication/Lanes.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>

namespace hal
{
    // A bounded byte channel to the other physical processor. No application
    // messages, coroutine state or RTOS objects cross this interface.
    class IInterCoreChannel
    {
      public:
        using Lane = communication::Lane;
        enum class Error : std::uint8_t { WouldBlock, Disconnected, TooLarge, BufferTooSmall };
        using Notification = std::move_only_function<void() noexcept>;

        virtual ~IInterCoreChannel() = default;
        IInterCoreChannel(const IInterCoreChannel&) = delete;
        IInterCoreChannel& operator=(const IInterCoreChannel&) = delete;
        IInterCoreChannel(IInterCoreChannel&&) = delete;
        IInterCoreChannel& operator=(IInterCoreChannel&&) = delete;
        // One producer/consumer per lane; neither operation waits. Errors leave
        // buffers/queues untouched. All lanes have independent capacity.
        // Receive returns WouldBlock while a higher-priority lane has queued
        // frames. Servicing that lane must notify local waiters as well as the peer.
        virtual std::expected<void, Error> trySend(std::span<const std::byte> frame, Lane lane) noexcept = 0;
        virtual std::expected<std::size_t, Error> tryReceive(std::span<std::byte> buffer, Lane lane) noexcept = 0;
        // Callback may run in an ISR: only notify a local worker. Registration
        // and clearing are synchronized with the ISR; setting also signals once
        // so data queued before registration cannot be missed.
        virtual void setNotification(Notification callback) noexcept = 0;
        // Permanent for this boot/endpoint; wake the peer and local waiters.
        virtual void close() noexcept = 0;
      protected:
        IInterCoreChannel() = default;
    };
}
