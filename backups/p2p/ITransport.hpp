#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace p2p
{
    using Bytes = std::vector<std::byte>;
    enum class Lane : std::uint8_t
    {
        Normal,
        Urgent
    };
    enum class SendResult : std::uint8_t
    {
        Sent,
        Full,
        Disconnected
    };
    struct Capacity
    {
        std::size_t available_frames{}, maximum_frame_bytes{};
    };

    // Reliable, ordered complete frames within each lane, until disconnection.
    // Urgent frames may overtake normal frames. Each endpoint has one IO owner.
    // Notifications may coalesce; receive() is the source of truth. Callbacks only
    // schedule work, never dispatch inline, and must not throw. No ISR calls here.
    class ITransport
    {
      public:
        ITransport() = default;
        ITransport(const ITransport&) = delete;
        ITransport& operator=(const ITransport&) = delete;
        ITransport(ITransport&&) = delete;
        ITransport& operator=(ITransport&&) = delete;
        virtual ~ITransport() = default;
        virtual SendResult trySend(std::span<const std::byte> frame, Lane lane) = 0;
        virtual bool tryReceive(Bytes& frame) = 0;
        virtual bool connected() const = 0;
        virtual Capacity capacity(Lane lane) const = 0;
        // Empty callback disconnects notifications; an in-flight invocation may finish.
        virtual void setWakeup(std::function<void()> callback) = 0;
    };
}
