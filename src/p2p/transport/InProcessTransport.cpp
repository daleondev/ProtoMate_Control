#include "InProcessTransport.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>

namespace p2p
{
    struct InProcessTransport::Shared
    {
        struct Frame
        {
            std::array<std::byte, MAX_FRAME_BYTES> bytes{};
            std::size_t size{};
        };

        struct Queue
        {
            std::array<Frame, FRAME_CAPACITY> frames;
            std::size_t head{};
            std::size_t count{};

            // Each blocked operation installs a fresh signal while holding mutex.
            // Closing that signal wakes it even if it has not started awaiting yet.
            // No notification tokens accumulate while neither operation is waiting.
            std::optional<pnm::coro::Channel<bool>> dataReady;
            std::optional<pnm::coro::Channel<bool>> spaceReady;
        };

        std::mutex mutex;
        std::array<Queue, 2> inbox;
        bool closed{};
    };

    InProcessTransport::InProcessTransport(std::shared_ptr<Shared> shared, std::size_t side)
      : m_shared(std::move(shared))
      , m_side(side)
    {
    }

    InProcessTransport::~InProcessTransport() { close(); }

    std::array<std::shared_ptr<InProcessTransport>, 2> InProcessTransport::pair()
    {
        auto shared = std::make_shared<Shared>();
        return { std::shared_ptr<InProcessTransport>(new InProcessTransport(shared, 0)),
                 std::shared_ptr<InProcessTransport>(new InProcessTransport(shared, 1)) };
    }

    pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> InProcessTransport::send(
      std::span<const std::byte> frame,
      std::stop_token stop)
    {
        auto shared = m_shared;
        auto& queue = shared->inbox[1 - m_side];
        std::unique_lock lock(shared->mutex);
        for (;;) {
            if (stop.stop_requested())
                co_return std::unexpected(Error::Cancelled);
            if (shared->closed)
                co_return std::unexpected(Error::Disconnected);
            if (frame.size() > MAX_FRAME_BYTES)
                co_return std::unexpected(Error::TooLarge);
            if (queue.count < FRAME_CAPACITY)
                break;

            pnm::coro::Channel<bool> ready;
            queue.spaceReady = ready;
            lock.unlock();
            co_await ready.next(stop);
            lock.lock();
            queue.spaceReady.reset();
            // Notifications and cancellation both recheck the protected state.
        }

        auto& slot = queue.frames[(queue.head + queue.count) % FRAME_CAPACITY];
        std::ranges::copy(frame, slot.bytes.begin());
        slot.size = frame.size();
        ++queue.count;
        auto wake = std::exchange(queue.dataReady, std::nullopt);
        lock.unlock();
        // A wakeup may resume a coroutine inline. Never notify under the queue lock.
        if (wake)
            wake->close();
        co_return pnm::EmptyResult<Error>{};
    }

    pnm::coro::Task<pnm::Result<std::size_t, ITransport::Error>> InProcessTransport::receive(
      std::span<std::byte> buffer,
      std::stop_token stop)
    {
        auto shared = m_shared;
        auto& queue = shared->inbox[m_side];
        std::unique_lock lock(shared->mutex);
        for (;;) {
            if (stop.stop_requested())
                co_return std::unexpected(Error::Cancelled);
            if (shared->closed)
                co_return std::unexpected(Error::Disconnected);
            if (queue.count != 0)
                break;

            pnm::coro::Channel<bool> ready;
            queue.dataReady = ready;
            lock.unlock();
            co_await ready.next(stop);
            lock.lock();
            queue.dataReady.reset();
        }

        const auto& slot = queue.frames[queue.head];
        if (slot.size > buffer.size())
            co_return std::unexpected(Error::BufferTooSmall);

        const auto size = slot.size;
        std::ranges::copy(std::span(slot.bytes).first(size), buffer.begin());
        queue.head = (queue.head + 1) % FRAME_CAPACITY;
        --queue.count;
        auto wake = std::exchange(queue.spaceReady, std::nullopt);
        lock.unlock();
        if (wake)
            wake->close();
        co_return size;
    }

    void InProcessTransport::close() noexcept
    {
        std::array<std::optional<pnm::coro::Channel<bool>>, 4> wake;
        {
            std::scoped_lock lock(m_shared->mutex);
            if (m_shared->closed)
                return;
            m_shared->closed = true;
            for (std::size_t side = 0; side < m_shared->inbox.size(); ++side) {
                auto& queue = m_shared->inbox[side];
                queue.count = 0;
                wake[2 * side] = std::exchange(queue.dataReady, std::nullopt);
                wake[2 * side + 1] = std::exchange(queue.spaceReady, std::nullopt);
            }
        }
        for (auto& signal : wake)
            if (signal)
                signal->close();
    }
}
