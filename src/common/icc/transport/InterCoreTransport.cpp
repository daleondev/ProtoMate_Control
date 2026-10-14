#include "InterCoreTransport.hpp"
#include "runtime/thread.hpp"

#include <stdexcept>
#include <utility>

namespace
{
    icc::ITransport::Error transport_error(hal::IInterCoreChannel::Error error)
    {
        using Error = hal::IInterCoreChannel::Error;
        switch (error) {
            case Error::TooLarge:
                return icc::ITransport::Error::TooLarge;
            case Error::BufferTooSmall:
                return icc::ITransport::Error::BufferTooSmall;
            default:
                return icc::ITransport::Error::Disconnected;
        }
    }
}

namespace icc
{
    InterCoreTransport::InterCoreTransport(std::shared_ptr<hal::IInterCoreChannel> channel)
      : m_channel(std::move(channel))
    {
        if (!m_channel) {
            throw std::invalid_argument("Missing inter-core channel");
        }
        m_worker = runtime::thread::create({ .name = "intercore.notify", .priority = 3, .stack_size = 8192 },
                                           [this] { notificationThread(); });
        m_channel->setNotification([this]() noexcept { m_notification.signal(); });
    }

    InterCoreTransport::~InterCoreTransport()
    {
        // The bridge must have drained its tasks before destroying the transport.
        close();
        m_channel->setNotification({});
        m_stopping.store(true);
        m_notification.signal();
        m_worker.join();
    }

    void InterCoreTransport::notificationThread()
    {
        while (!m_stopping.load()) {
            m_notification.waitUntil(std::chrono::steady_clock::time_point::max());
            wakeWaiters();
        }
    }

    void InterCoreTransport::wakeWaiters()
    {
        std::array<std::optional<pnm::coro::Channel<bool>>, 2 * LANE_COUNT> wake;
        {
            const std::scoped_lock lock(m_mutex);
            const auto ready = m_channel->readiness();
            for (std::size_t priority{}; priority < LANE_COUNT; ++priority) {
                const auto lane = static_cast<std::size_t>(LANE_PRIORITY[priority]);
                const auto index = 2 * lane;
                for (std::size_t operation{}; operation < 2; ++operation) {
                    auto& waiter = m_waiters[index + operation];
                    const bool available = operation == 0 ? ready.send[lane] : ready.receive[lane];
                    if (waiter.waiting && !waiter.pending && (m_closed || available)) {
                        waiter.waiting = false;
                        waiter.pending = true;
                        wake[2 * priority + operation] = waiter.ready;
                    }
                }
            }
        }
        for (auto& signal : wake) {
            if (signal) {
                signal->push(true);
            }
        }
    }

    void InterCoreTransport::close() noexcept
    {
        {
            const std::scoped_lock lock(m_mutex);
            if (m_closed) {
                return;
            }
            m_closed = true;
            m_channel->close();
        }
        m_notification.signal();
    }

    pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> InterCoreTransport::send(
      std::span<const std::byte> frame,
      Lane lane,
      std::stop_token stop)
    {
        const auto index = 2 * static_cast<std::size_t>(lane);
        if (index >= m_waiters.size()) { co_return std::unexpected(Error::Disconnected); }
        auto& waiter = m_waiters.at(index);
        for (;;) {
            std::unique_lock lock(m_mutex);
            if (stop.stop_requested()) {
                co_return std::unexpected(Error::Cancelled);
            }
            if (m_closed) {
                co_return std::unexpected(Error::Disconnected);
            }
            const auto result = m_channel->trySend(frame, lane);
            if (result) {
                co_return pnm::EmptyResult<Error>{};
            }
            if (result.error() != hal::IInterCoreChannel::Error::WouldBlock) {
                co_return std::unexpected(transport_error(result.error()));
            }
            // Installing under the same lock as the worker's exchange prevents
            // a notification between the failed operation and arming the wait.
            waiter.waiting = true;
            lock.unlock();
            const auto notified = co_await waiter.ready.wait(stop);
            lock.lock();
            waiter.waiting = false;
            if (notified) { waiter.pending = false; }
        }
    }

    pnm::coro::Task<pnm::Result<std::size_t, ITransport::Error>> InterCoreTransport::receive(
      std::span<std::byte> buffer,
      Lane lane,
      std::stop_token stop)
    {
        const auto index = 2 * static_cast<std::size_t>(lane) + 1;
        if (index >= m_waiters.size()) { co_return std::unexpected(Error::Disconnected); }
        auto& waiter = m_waiters.at(index);
        for (;;) {
            std::unique_lock lock(m_mutex);
            if (stop.stop_requested()) {
                co_return std::unexpected(Error::Cancelled);
            }
            if (m_closed) {
                co_return std::unexpected(Error::Disconnected);
            }
            const auto result =
              m_channel->tryReceive(buffer, lane);
            if (result) {
                co_return *result;
            }
            if (result.error() != hal::IInterCoreChannel::Error::WouldBlock) {
                co_return std::unexpected(transport_error(result.error()));
            }
            waiter.waiting = true;
            lock.unlock();
            const auto notified = co_await waiter.ready.wait(stop);
            lock.lock();
            waiter.waiting = false;
            if (notified) { waiter.pending = false; }
        }
    }
}
