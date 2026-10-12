#pragma once

#include "communication/Lanes.hpp"
#include <pneumo/coroutines.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <stop_token>

namespace icc
{
    using communication::Lane;
    using communication::LANE_PRIORITY;
    using communication::LANE_COUNT;
    using communication::MAX_BACKGROUND_FRAME_BYTES;

    // One endpoint of a connection between two bridges. Transfers complete,
    // ordered byte frames within each lane. Message types belong to the bridge.
    // Lanes have independent capacity and waiters. Receives prefer queued urgent
    // frames, then normal, then background, at frame boundaries. A receiver must
    // service all lanes in use; sustained higher-priority traffic can starve logs.
    // One send and one receive PER LANE may be pending concurrently. Full lower
    // lanes never block higher lanes. Independent rings enqueue without waiting
    // for higher lanes to drain; priority is applied when consuming frames. Waiting
    // suspends the calling coroutine, leaving the context free to run other work.
    class ITransport
    {
      public:
        enum class Error : std::uint8_t
        {
            Cancelled,
            Disconnected,
            TooLarge,
            BufferTooSmall
        };

        ITransport() = default;
        ITransport(const ITransport&) = delete;
        ITransport& operator=(const ITransport&) = delete;
        ITransport(ITransport&&) = delete;
        ITransport& operator=(ITransport&&) = delete;
        virtual ~ITransport() = default;

        // Borrows the frame and waits asynchronously for send space. Keep its
        // memory alive and unchanged until the returned task completes.
        // Success means queued, not processed by the peer. An error means the
        // frame was not queued; cancellation cannot recall an accepted frame.
        virtual pnm::coro::Task<pnm::EmptyResult<Error>> send(std::span<const std::byte> frame,
                                                              Lane lane = Lane::Normal,
                                                              std::stop_token stop = {}) = 0;

        // Waits asynchronously for the next complete frame, writes it into buffer
        // and returns its byte count. Keep the buffer alive and exclusively
        // available to this operation until the returned task completes.
        // BufferTooSmall leaves the frame queued for a retry with a larger buffer.
        // Errors leave the buffer unchanged and do not consume a frame.
        virtual pnm::coro::Task<pnm::Result<std::size_t, Error>> receive(std::span<std::byte> buffer,
                                                                         Lane lane = Lane::Normal,
                                                                         std::stop_token stop = {}) = 0;

        // Both operations must wake when cancellation is requested, even when
        // the peer is idle. Completion, including cancellation, guarantees no
        // further access to the borrowed memory. The transport must outlive all
        // its pending operations.
    };
}
