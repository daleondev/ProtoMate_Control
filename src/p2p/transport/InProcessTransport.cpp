#include "InProcessTransport.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>

// Coroutine lowering accesses static promise hooks through the promise object.
// NOLINTBEGIN(readability-static-accessed-through-instance)
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
            std::optional<pnm::coro::Channel<bool>> data_ready;
            std::optional<pnm::coro::Channel<bool>> space_ready;
        };

        std::mutex mutex;
        std::array<std::array<Queue, 2>, 2> inbox;
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
      Lane lane,
      std::stop_token stop)
    {
        auto shared = m_shared;
        auto& queue = shared->inbox[1 - m_side].at(static_cast<std::size_t>(lane));
        std::unique_lock lock(shared->mutex);
        for (;;) {
            if (stop.stop_requested()) {
                co_return std::unexpected(Error::Cancelled);
            }
            if (shared->closed) {
                co_return std::unexpected(Error::Disconnected);
            }
            if (frame.size() > MAX_FRAME_BYTES) {
                co_return std::unexpected(Error::TooLarge);
            }
            if (queue.count < FRAME_CAPACITY) {
                break;
            }

            pnm::coro::Channel<bool> ready;
            queue.space_ready = ready;
            lock.unlock();
            co_await ready.next(stop);
            lock.lock();
            queue.space_ready.reset();
            // Notifications and cancellation both recheck the protected state.
        }

        auto& slot = queue.frames[(queue.head + queue.count) % FRAME_CAPACITY];
        std::ranges::copy(frame, slot.bytes.begin());
        slot.size = frame.size();
        ++queue.count;
        auto wake = std::exchange(queue.data_ready, std::nullopt);
        lock.unlock();
        // A wakeup may resume a coroutine inline. Never notify under the queue lock.
        if (wake) {
            wake->close();
        }
        co_return pnm::EmptyResult<Error>{};
    }

    pnm::coro::Task<pnm::Result<std::size_t, ITransport::Error>> InProcessTransport::receive(
      std::span<std::byte> buffer,
      Lane lane,
      std::stop_token stop)
    {
        auto shared = m_shared;
        auto& queue = shared->inbox[m_side].at(static_cast<std::size_t>(lane));
        std::unique_lock lock(shared->mutex);
        for (;;) {
            if (stop.stop_requested()) {
                co_return std::unexpected(Error::Cancelled);
            }
            if (shared->closed) {
                co_return std::unexpected(Error::Disconnected);
            }
            if (queue.count != 0) {
                break;
            }

            pnm::coro::Channel<bool> ready;
            queue.data_ready = ready;
            lock.unlock();
            co_await ready.next(stop);
            lock.lock();
            queue.data_ready.reset();
        }

        const auto& slot = queue.frames[queue.head];
        if (slot.size > buffer.size()) {
            co_return std::unexpected(Error::BufferTooSmall);
        }

        const auto size = slot.size;
        std::ranges::copy(std::span(slot.bytes).first(size), buffer.begin());
        queue.head = (queue.head + 1) % FRAME_CAPACITY;
        --queue.count;
        auto wake = std::exchange(queue.space_ready, std::nullopt);
        lock.unlock();
        if (wake) {
            wake->close();
        }
        co_return size;
    }

    void InProcessTransport::close() noexcept
    {
        constexpr std::size_t signal_count{ 8 };
        std::array<std::optional<pnm::coro::Channel<bool>>, signal_count> wake;
        {
            std::scoped_lock lock(m_shared->mutex);
            if (m_shared->closed) {
                return;
            }
            m_shared->closed = true;
            for (std::size_t side = 0; side < m_shared->inbox.size(); ++side) {
                for (std::size_t lane = 0; lane < 2; ++lane) {
                    auto& queue = m_shared->inbox[side][lane];
                    queue.count = 0;
                    const auto index = (4 * side) + (2 * lane);
                    wake[index] = std::exchange(queue.data_ready, std::nullopt);
                    wake[index + 1] = std::exchange(queue.space_ready, std::nullopt);
                }
            }
        }
        for (auto& signal : wake) {
            if (signal) {
                signal->close();
            }
        }
    }
}

// NOLINTEND(readability-static-accessed-through-instance)
