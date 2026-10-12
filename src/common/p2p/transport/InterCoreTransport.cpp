#include "InterCoreTransport.hpp"
#include "runtime/thread.hpp"

#include <stdexcept>
#include <utility>

namespace
{
    p2p::ITransport::Error transport_error(hal::IInterCoreChannel::Error error)
    {
        using Error = hal::IInterCoreChannel::Error;
        switch (error) {
            case Error::TooLarge: return p2p::ITransport::Error::TooLarge;
            case Error::BufferTooSmall: return p2p::ITransport::Error::BufferTooSmall;
            default: return p2p::ITransport::Error::Disconnected;
        }
    }
}

namespace p2p
{
    InterCoreTransport::InterCoreTransport(std::shared_ptr<hal::IInterCoreChannel> channel)
      : m_channel(std::move(channel))
    {
        if (!m_channel) { throw std::invalid_argument("Missing inter-core channel"); }
        m_worker = runtime::thread::create(
          {.name = "intercore.notify", .priority = 3, .stack_size = 8192},
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
        decltype(m_waiters) wake;
        {
            const std::scoped_lock lock(m_mutex);
            for (std::size_t index{}; index < wake.size(); ++index) {
                wake[index] = std::exchange(m_waiters[index], std::nullopt);
            }
        }
        for (auto& signal : wake) {
            if (signal) { signal->close(); }
        }
    }

    void InterCoreTransport::close() noexcept
    {
        {
            const std::scoped_lock lock(m_mutex);
            if (m_closed) { return; }
            m_closed = true;
            m_channel->close();
        }
        m_notification.signal();
    }

    pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> InterCoreTransport::send(
      std::span<const std::byte> frame, Lane lane, std::stop_token stop)
    {
        const auto index = 2 * static_cast<std::size_t>(lane);
        for (;;) {
            pnm::coro::Channel<bool> ready;
            std::unique_lock lock(m_mutex);
            if (stop.stop_requested()) { co_return std::unexpected(Error::Cancelled); }
            if (m_closed) { co_return std::unexpected(Error::Disconnected); }
            const auto result = m_channel->trySend(frame, static_cast<hal::IInterCoreChannel::Lane>(lane));
            if (result) { co_return pnm::EmptyResult<Error>{}; }
            if (result.error() != hal::IInterCoreChannel::Error::WouldBlock) {
                co_return std::unexpected(transport_error(result.error()));
            }
            // Installing under the same lock as the worker's exchange prevents
            // a notification between the failed operation and arming the wait.
            m_waiters.at(index) = ready;
            lock.unlock();
            co_await ready.next(stop);
            lock.lock();
            m_waiters.at(index).reset();
        }
    }

    pnm::coro::Task<pnm::Result<std::size_t, ITransport::Error>> InterCoreTransport::receive(
      std::span<std::byte> buffer, Lane lane, std::stop_token stop)
    {
        const auto index = 2 * static_cast<std::size_t>(lane) + 1;
        for (;;) {
            pnm::coro::Channel<bool> ready;
            std::unique_lock lock(m_mutex);
            if (stop.stop_requested()) { co_return std::unexpected(Error::Cancelled); }
            if (m_closed) { co_return std::unexpected(Error::Disconnected); }
            const auto result = m_channel->tryReceive(buffer, static_cast<hal::IInterCoreChannel::Lane>(lane));
            if (result) { co_return *result; }
            if (result.error() != hal::IInterCoreChannel::Error::WouldBlock) {
                co_return std::unexpected(transport_error(result.error()));
            }
            m_waiters.at(index) = ready;
            lock.unlock();
            co_await ready.next(stop);
            lock.lock();
            m_waiters.at(index).reset();
        }
    }
}
