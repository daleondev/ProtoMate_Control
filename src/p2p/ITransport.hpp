#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

namespace p2p
{
    using Bytes = std::vector<std::byte>;
    enum class Lane
    {
        Normal,
        Urgent
    };
    enum class SendResult
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
        virtual ~ITransport() = default;
        virtual SendResult trySend(std::span<const std::byte> frame, Lane lane) = 0;
        virtual bool tryReceive(Bytes& frame) = 0;
        virtual bool connected() const = 0;
        virtual Capacity capacity(Lane lane) const = 0;
        // Empty callback disconnects notifications; an in-flight invocation may finish.
        virtual void setWakeup(std::function<void()> callback) = 0;
    };
}
