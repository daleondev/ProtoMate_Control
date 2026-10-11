#pragma once

#include "ITransport.hpp"

#include <array>
#include <cstddef>
#include <memory>

namespace p2p
{
    // Two connected endpoints with a bounded FIFO in each direction.
    // The shared queues own copies of the frames, never the caller's spans.
    class InProcessTransport final : public ITransport
    {
      public:
        static constexpr std::size_t FRAME_CAPACITY{ 8 };
        static constexpr std::size_t MAX_FRAME_BYTES{ 2048 };

        // Allocate both queues' frame storage once. Coroutine waits may still
        // allocate their task and notification state.
        static std::array<std::shared_ptr<InProcessTransport>, 2> pair();
        ~InProcessTransport() override;

        InProcessTransport(const InProcessTransport&) = delete;
        InProcessTransport& operator=(const InProcessTransport&) = delete;
        InProcessTransport(InProcessTransport&&) = delete;
        InProcessTransport& operator=(InProcessTransport&&) = delete;

        // As with ITransport, allow one send and one receive per endpoint at a time.
        pnm::coro::Task<pnm::EmptyResult<Error>> send(std::span<const std::byte> frame,
                                                      std::stop_token stop = {}) override;
        pnm::coro::Task<pnm::Result<std::size_t, Error>> receive(std::span<std::byte> buffer,
                                                                 std::stop_token stop = {}) override;

        // Permanently disconnect both endpoints, discard queued frames and wake
        // waiters. Thread-safe and idempotent; destruction also closes the pair.
        // Keep endpoints and their contexts alive until pending tasks have finished.
        void close() noexcept;

      private:
        struct Shared;
        InProcessTransport(std::shared_ptr<Shared> shared, std::size_t side);

        std::shared_ptr<Shared> m_shared;
        std::size_t m_side;
    };
}
