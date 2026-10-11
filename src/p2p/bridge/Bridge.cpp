#include "Bridge.hpp"

#include <stdexcept>
#include <utility>

namespace p2p
{
    Bridge::Bridge(pnm::msg::Bus& bus, pnm::coro::Context& context, ITransport& transport)
      : m_context(context)
      , m_transport(transport)
      , m_topics(bus, m_outgoingWakeup)
      , m_services(bus, context, m_outgoingWakeup, m_stopSource.get_token())
    {
    }

    Bridge::~Bridge() { stop(); }

    void Bridge::start()
    {
        std::unique_lock lock(m_mutex);
        if (m_started)
            throw std::logic_error("Bridge can only be started once");
        m_started = true;

        try {
            // Construct both coroutine chains before scheduling either one.
            // Moving these ready-made tasks into co_spawn also avoids allocating
            // a new loop task inside its detached entry callback.
            auto sender = runLoop(sendLoop(m_stopSource.get_token()), m_sendRunning);
            auto receiver = runLoop(receiveLoop(m_stopSource.get_token()), m_receiveRunning);

            pnm::coro::co_spawn(m_context, std::move(sender));
            m_sendRunning = true;

            pnm::coro::co_spawn(m_context, std::move(receiver));
            m_receiveRunning = true;
            // Holding the mutex prevents an immediately completing handler from
            // clearing its flag before its scheduling has been recorded.
        } catch (...) {
            m_failure = std::current_exception();
            lock.unlock();
            // If only the first handler was scheduled, it must still finish.
            // Keep driving the context and call stop() before destruction.
            m_stopSource.request_stop();
            throw;
        }
    }

    void Bridge::stop()
    {
        {
            std::scoped_lock lock(m_mutex);
            if (!m_started)
                return;
        }

        m_stopSource.request_stop();

        std::unique_lock lock(m_mutex);
        m_finished.wait(lock, [this] { return !m_sendRunning && !m_receiveRunning; });
        m_topics.close();
        m_outgoingWakeup->ready.close();
    }

    std::optional<Bridge::Failure> Bridge::failure() const
    {
        std::scoped_lock lock(m_mutex);
        return m_failure;
    }

    void Bridge::checkRegistration() const
    {
        if (m_started)
            throw std::logic_error("Register bridge routes before start");
    }

    pnm::coro::Task<std::optional<Bridge::OutgoingFrame>> Bridge::nextOutgoingFrame(std::stop_token stop)
    {
        while (!stop.stop_requested()) {
            m_services.poll();
            if (m_preferService) {
                if (auto frame = m_services.takeFrame()) {
                    m_preferService = false;
                    co_return frame;
                }
            }
            if (auto frame = m_topics.takeFrame()) {
                m_preferService = true;
                co_return frame;
            }

            if (!m_preferService) {
                if (auto frame = m_services.takeFrame())
                    co_return frame;
            }

            if (!(co_await m_outgoingWakeup->ready.next(stop))) {
                if (stop.stop_requested())
                    co_return std::nullopt;
                throw std::runtime_error("Bridge outgoing notification failed");
            }
            // Rescan after clearing the signal. A publication racing this clear
            // either remains in its subscription or supplies the next wakeup.
            m_outgoingWakeup->pending.clear();
        }
        co_return std::nullopt;
    }

    void Bridge::dispatchIncomingFrame(std::span<const std::byte> bytes)
    {
        const auto frame = util::decodeFrame(bytes);
        if (frame.kind == util::FrameKind::Topic)
            m_topics.dispatch(frame);
        else
            m_services.dispatch(frame);
    }

    pnm::coro::Task<void> Bridge::runLoop(pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> loop,
                                          bool& running)
    {
        std::optional<Failure> failure;
        try {
            auto result = co_await loop;
            if (!result)
                failure = result.error();
        } catch (...) {
            failure = std::current_exception();
        }

        // Awaiting completion does not itself destroy the owned task. Release
        // its frame before stop() is allowed to return or the bridge can die.
        loop = {};

        {
            std::scoped_lock lock(m_mutex);
            if (failure && !m_failure)
                m_failure = std::move(failure);
        }
        m_stopSource.request_stop();

        // This is our last access to the bridge. Notify while holding the lock
        // so a waking destructor cannot destroy the condition variable early.
        std::scoped_lock lock(m_mutex);
        running = false;
        if (!m_sendRunning && !m_receiveRunning)
            m_services.close(m_failure ? pnm::msg::ServiceError::TransportError
                                       : pnm::msg::ServiceError::Cancelled);
        m_finished.notify_all();
    }

    pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> Bridge::sendLoop(std::stop_token stop)
    {
        while (!stop.stop_requested()) {
            // Suspends until a topic, service event or deadline provides work.
            auto frame = co_await nextOutgoingFrame(stop);
            if (!frame || stop.stop_requested())
                break;

            if (frame->size > frame->bytes.size())
                throw std::length_error("Outgoing bridge frame exceeds its buffer");

            // frame owns the storage throughout the await. A full transport
            // suspends this loop while the receive loop can continue running.
            const auto bytes = std::span<const std::byte>(frame->bytes).first(frame->size);
            auto result = co_await m_transport.send(bytes, stop);
            if (!result) {
                if (result.error() == ITransport::Error::Cancelled && stop.stop_requested())
                    break;
                co_return std::unexpected(result.error());
            }
        }
        co_return pnm::EmptyResult<ITransport::Error>{};
    }

    pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> Bridge::receiveLoop(std::stop_token stop)
    {
        // This buffer lives in the coroutine frame across suspension and is
        // reused only after the previous frame has been dispatched.
        std::array<std::byte, util::MAX_FRAME_BYTES> buffer{};
        while (!stop.stop_requested()) {
            auto result = co_await m_transport.receive(buffer, stop);
            if (!result) {
                if (result.error() == ITransport::Error::Cancelled && stop.stop_requested())
                    break;
                // Includes BufferTooSmall: return the failure instead of
                // repeatedly retrying the same frame with the same buffer.
                co_return std::unexpected(result.error());
            }
            if (stop.stop_requested())
                break;

            if (*result > buffer.size())
                throw std::length_error("Transport returned a frame larger than the receive buffer");

            dispatchIncomingFrame(std::span<const std::byte>(buffer).first(*result));
        }
        co_return pnm::EmptyResult<ITransport::Error>{};
    }
}
