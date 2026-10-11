#pragma once

#include <pneumo/coroutines.hpp>

#include <cstddef>
#include <expected>
#include <span>
#include <stop_token>

namespace p2p
{
    // One endpoint of a connection between two bridges. Transfers complete,
    // ordered byte frames; message types and topic names belong to the bridge.
    // One send and one receive may be pending concurrently. Waiting suspends
    // the calling coroutine, leaving the context free to run other work.
    class ITransport
    {
      public:
        enum class Error
        {
            Cancelled,
            Disconnected,
            TooLarge,
            BufferTooSmall
        };

        virtual ~ITransport() = default;

        // Borrows the frame and waits asynchronously for send space. Keep its
        // memory alive and unchanged until the returned task completes.
        // Success means queued, not processed by the peer. An error means the
        // frame was not queued; cancellation cannot recall an accepted frame.
        virtual pnm::coro::Task<pnm::EmptyResult<Error>> send(std::span<const std::byte> frame,
                                                              std::stop_token stop = {}) = 0;

        // Waits asynchronously for the next complete frame, writes it into buffer
        // and returns its byte count. Keep the buffer alive and exclusively
        // available to this operation until the returned task completes.
        // BufferTooSmall leaves the frame queued for a retry with a larger buffer.
        // Errors leave the buffer unchanged and do not consume a frame.
        virtual pnm::coro::Task<pnm::Result<std::size_t, Error>> receive(std::span<std::byte> buffer,
                                                                         std::stop_token stop = {}) = 0;

        // Both operations must wake when cancellation is requested, even when
        // the peer is idle. Completion, including cancellation, guarantees no
        // further access to the borrowed memory. The transport must outlive all
        // its pending operations.
    };
}
