#include "Bridge.hpp"

#include <stdexcept>
#include <utility>

// Coroutine lowering accesses static promise hooks through the promise object.
// NOLINTBEGIN(readability-static-accessed-through-instance)
namespace p2p
{
    namespace
    {
        // Bound each dispatch turn even when the transport completes immediately.
        // The context has one runner; yielding lets the other lane run under load.
        // Awaiter hook names are prescribed by the C++ coroutine protocol.
        // NOLINTBEGIN(readability-identifier-naming)
        struct YieldToContext
        {
            pnm::coro::Context* context;
            static bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<> handle) const { context->schedule(handle); }
            static void await_resume() noexcept {}
        };
        // NOLINTEND(readability-identifier-naming)
    }

    Bridge::Bridge(pnm::msg::Bus& bus,
                   pnm::coro::Context& context,
                   ITransport& transport,
                   ConnectionOptions connection)
      : m_context(context)
      , m_transport(transport)
      , m_topics(bus, m_outgoingWakeup)
      , m_services(bus, context, m_outgoingWakeup, m_stopSource.get_token())
      , m_actions(bus, context, m_outgoingWakeup, m_stopSource.get_token())
      , m_connection(connection)
    {
    }

    Bridge::~Bridge() { stop(); }

    void Bridge::start()
    {
        std::unique_lock lock(m_mutex);
        if (m_started) {
            throw std::logic_error("Bridge can only be started once");
        }
        m_started = true;

        try {
            // Construct all coroutine chains before scheduling any of them.
            // Moving these ready-made tasks into co_spawn also avoids allocating
            // a new loop task inside its detached entry callback.
            auto health = runLoop(healthLoop(m_stopSource.get_token()), &m_healthRunning);
            auto sender = runLoop(sendLoop(Lane::Normal, m_stopSource.get_token()), &m_sendRunning);
            auto receiver = runLoop(receiveLoop(Lane::Normal, m_stopSource.get_token()), &m_receiveRunning);

            auto urgent_sender =
              runLoop(sendLoop(Lane::Urgent, m_stopSource.get_token()), &m_urgentSendRunning);
            auto urgent_receiver =
              runLoop(receiveLoop(Lane::Urgent, m_stopSource.get_token()), &m_urgentReceiveRunning);

            pnm::coro::co_spawn(m_context, std::move(health));
            m_healthRunning = true;
            pnm::coro::co_spawn(m_context, std::move(urgent_sender));
            m_urgentSendRunning = true;
            pnm::coro::co_spawn(m_context, std::move(urgent_receiver));
            m_urgentReceiveRunning = true;
            pnm::coro::co_spawn(m_context, std::move(sender));
            m_sendRunning = true;

            pnm::coro::co_spawn(m_context, std::move(receiver));
            m_receiveRunning = true;
            // Holding the mutex prevents an immediately completing handler from
            // clearing its flag before its scheduling has been recorded.
        } catch (...) {
            m_failure = std::current_exception();
            m_failureReason = ConnectionReason::InternalError;
            lock.unlock();
            // If only the first handler was scheduled, it must still finish.
            // Keep driving the context and call stop() before destruction.
            m_stopSource.request_stop();
            throw;
        }
    }

    void Bridge::stop(std::chrono::milliseconds grace)
    {
        {
            std::scoped_lock lock(m_mutex);
            if (!m_started) {
                return;
            }
        }

        m_shutdownRequested.store(true);
        m_outgoingWakeup->notify();
        m_healthWakeup->notify();
        std::unique_lock lock(m_mutex);
        const auto stopped = [this] { return loopsStopped(); };
        if (!m_finished.wait_for(lock, grace, stopped)) {
            // The grace period bounds action draining, not an already-running
            // final notification. Once cleanup starts, wait for that callback.
            if (!m_failure && !m_cleanupRunning) {
                m_failure = std::make_exception_ptr(
                  std::runtime_error("Bridge shutdown timed out; action completion is unknown"));
                m_failureReason = ConnectionReason::ShutdownTimeout;
            }
            lock.unlock();
            m_stopSource.request_stop();
            lock.lock();
            m_finished.wait(lock, stopped);
        }
        m_topics.close();
        m_outgoingWakeup->close();
        m_healthWakeup->ready.close();
    }

    bool Bridge::loopsStopped() const
    {
        return !m_sendRunning && !m_receiveRunning && !m_urgentSendRunning && !m_urgentReceiveRunning &&
               !m_healthRunning && !m_cleanupRunning;
    }

    std::optional<Bridge::Failure> Bridge::failure() const
    {
        std::scoped_lock lock(m_mutex);
        return m_failure;
    }

    void Bridge::setConnectionCallback(ConnectionCallback callback)
    {
        std::scoped_lock lock(m_mutex);
        checkRegistration();
        m_connection.setCallback(std::move(callback));
    }

    ConnectionStatus Bridge::connectionStatus() const { return m_connection.status(); }

    void Bridge::beginShutdown()
    {
        m_connection.transition({ .state = ConnectionState::Stopping, .reason =
                                  m_peerStopped ? ConnectionReason::PeerStopped : ConnectionReason::None });
        m_actions.requestStop();
    }

    void Bridge::recordFailure(Failure failure, ConnectionReason reason)
    {
        std::scoped_lock lock(m_mutex);
        if (!m_failure) {
            m_failure = std::move(failure);
            m_failureReason = reason;
        }
    }

    pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> Bridge::healthLoop(std::stop_token stop)
    {
        m_connection.begin();
        m_outgoingWakeup->notify();
        while (!stop.stop_requested()) {
            if (m_shutdownRequested.load()) {
                beginShutdown();
            }
            if (m_connection.tick()) {
                m_outgoingWakeup->notify();
            }
            auto timer =
              m_context.scheduleAt(m_connection.nextWake(), [signal = m_healthWakeup] { signal->notify(); });
            if (!(co_await m_healthWakeup->ready.next(stop))) {
                if (stop.stop_requested()) {
                    break;
                }
                throw std::runtime_error("Connection health notification failed");
            }
            m_healthWakeup->pending.clear();
        }
        co_return pnm::EmptyResult<ITransport::Error>{};
    }

    void Bridge::checkRegistration() const
    {
        if (m_started) {
            throw std::logic_error("Register bridge routes before start");
        }
    }

    pnm::coro::Task<std::optional<Bridge::OutgoingFrame>> Bridge::nextOutgoingFrame(Lane lane,
                                                                                    std::stop_token stop)
    {
        while (!stop.stop_requested()) {
            if (m_shutdownRequested.load()) {
                beginShutdown();
            }
            m_connection.checkDeadline();
            if (m_connection.ready()) {
                m_services.poll();
                m_actions.poll();
            }
            if (m_shutdownRequested.load() && m_actions.drained() && !m_actionSending[0] &&
                !m_actionSending[1]) {
                if (m_connection.goodbyeSent()) {
                    m_stopSource.request_stop();
                    co_return std::nullopt;
                }
                m_connection.requestGoodbye();
                m_outgoingWakeup->signal(Lane::Urgent).notify();
            }
            if (auto frame = takeReadyFrame(lane)) {
                co_return frame;
            }

            if (!(co_await m_outgoingWakeup->signal(lane).ready.next(stop))) {
                if (stop.stop_requested()) {
                    co_return std::nullopt;
                }
                throw std::runtime_error("Bridge outgoing notification failed");
            }
            // Rescan after clearing the signal. A publication racing this clear
            // either remains in its subscription or supplies the next wakeup.
            m_outgoingWakeup->signal(lane).pending.clear();
        }
        co_return std::nullopt;
    }

    std::optional<Bridge::OutgoingFrame> Bridge::takeReadyFrame(Lane lane)
    {
        if (lane == Lane::Urgent) {
            if (auto frame = m_connection.takeFrame()) {
                m_healthWakeup->notify();
                return frame;
            }
        }
        if (m_connection.ready()) {
            for (unsigned checked = 0; checked < 3; ++checked) {
                if (auto frame = takeFrame(lane, m_nextHelper[static_cast<std::size_t>(lane)]++ % 3)) {
                    const auto kind = static_cast<util::FrameKind>(frame->bytes[1]);
                    m_actionSending[static_cast<std::size_t>(lane)] =
                      kind >= util::FrameKind::ActionGoal && kind <= util::FrameKind::ActionAck;
                    return frame;
                }
            }
        }

        return std::nullopt;
    }

    std::optional<Bridge::OutgoingFrame> Bridge::takeFrame(Lane lane, std::size_t helper)
    {
        // Return directly so each 2 KiB frame is constructed in the caller's
        // storage, avoiding one stack temporary per switch branch in the coroutine.
        switch (helper) {
            case 0:
                return m_actions.takeFrame(lane);
            case 1:
                return m_services.takeFrame(lane);
            default:
                return lane == Lane::Normal ? m_topics.takeFrame() : std::nullopt;
        }
    }

    void Bridge::dispatchIncomingFrame(std::span<const std::byte> bytes, Lane lane)
    {
        m_connection.checkDeadline();
        const auto frame = util::decode_frame(bytes);
        if (frame.kind >= util::FrameKind::Hello) {
            if (m_connection.dispatch(frame, lane)) {
                m_peerStopped = true;
                if (!m_actions.drained()) {
                    throw util::ConnectionFailure(ConnectionReason::PeerStopped,
                                                  "Peer stopped with outstanding actions");
                }
                m_connection.transition({ .state = ConnectionState::Stopping, .reason = ConnectionReason::PeerStopped });
                m_stopSource.request_stop();
            }
            m_healthWakeup->notify();
            m_outgoingWakeup->notify();
            return;
        }
        if (!m_connection.ready()) {
            throw std::invalid_argument("Application frame before handshake completion");
        }
        if (frame.kind == util::FrameKind::Topic) {
            if (lane != Lane::Normal) {
                throw std::invalid_argument("Topics cannot use the urgent lane");
            }
            m_topics.dispatch(frame);
        }
        else if (frame.kind >= util::FrameKind::ActionGoal && frame.kind <= util::FrameKind::ActionAck) {
            m_actions.dispatch(frame, lane);
        }
        else {
            m_services.dispatch(frame, lane);
        }
    }

    pnm::coro::Task<void> Bridge::runLoop(pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> loop,
                                          bool* running)
    {
        try {
            auto result = co_await loop;
            if (!result) {
                recordFailure(result.error(), ConnectionReason::TransportError);
            }
        } catch (const util::ConnectionFailure& error) {
            recordFailure(std::current_exception(), error.reason);
        } catch (const std::invalid_argument&) {
            recordFailure(std::current_exception(), ConnectionReason::ProtocolError);
        } catch (...) {
            recordFailure(std::current_exception(), ConnectionReason::InternalError);
        }
        // Release the coroutine and borrowed transport memory before stop returns.
        loop = {};
        m_stopSource.request_stop();
        bool last{};
        {
            std::scoped_lock lock(m_mutex);
            *running = false;
            last = loopsStopped();
            if (last) {
                m_cleanupRunning = true;
            }
            else {
                m_finished.notify_all();
            }
        }
        if (last) {
            // Application notifications must never run under the lifecycle lock.
            // m_cleanupRunning keeps stop/destruction waiting through this callback.
            finishConnection();
            std::scoped_lock lock(m_mutex);
            m_cleanupRunning = false;
            m_finished.notify_all();
        }
    }

    void Bridge::finishConnection()
    {
        try {
            const bool failed = failure().has_value();
            m_services.close(failed ? pnm::msg::ServiceError::TransportError
                                    : pnm::msg::ServiceError::Cancelled);
            m_actions.close(failed ? pnm::msg::ActionError::TransportError
                                   : pnm::msg::ActionError::Unavailable);
            m_topics.close();
            ConnectionStatus status;
            {
                std::scoped_lock lock(m_mutex);
                status = m_failure ? ConnectionStatus{ .state = ConnectionState::Failed, .reason = m_failureReason }
                                   : ConnectionStatus{ .state = ConnectionState::Stopped, .reason =
                                                       m_peerStopped ? ConnectionReason::PeerStopped
                                                                     : ConnectionReason::None };
            }
            m_connection.transition(status);
        } catch (...) {
            recordFailure(std::current_exception(), ConnectionReason::InternalError);
            ConnectionReason reason{};
            {
                std::scoped_lock lock(m_mutex);
                reason = m_failureReason;
            }
            try {
                m_connection.transition({ .state = ConnectionState::Failed, .reason = reason });
            } catch (...) {
                // The failure is recorded; a failing observer cannot interrupt teardown.
                return;
            }
        }
    }

    pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> Bridge::sendLoop(Lane lane, std::stop_token stop)
    {
        while (!stop.stop_requested()) {
            // Suspends until a message, lifecycle event or deadline provides work.
            auto frame = co_await nextOutgoingFrame(lane, stop);
            if (!frame || stop.stop_requested()) {
                break;
            }

            if (frame->size > frame->bytes.size()) {
                throw std::length_error("Outgoing bridge frame exceeds its buffer");
            }

            // frame owns the storage throughout the await. A full transport
            // suspends this loop while the receive loop can continue running.
            const auto bytes = std::span<const std::byte>(frame->bytes).first(frame->size);
            auto result = co_await m_transport.send(bytes, lane, stop);
            if (!result) {
                if (result.error() == ITransport::Error::Cancelled && stop.stop_requested()) {
                    break;
                }
                co_return std::unexpected(result.error());
            }
            m_connection.sent(static_cast<util::FrameKind>(frame->bytes[1]));
            m_actionSending[static_cast<std::size_t>(lane)] = false;
            if (m_shutdownRequested.load()) {
                m_outgoingWakeup->notify();
            }
            co_await YieldToContext{ .context = &m_context };
        }
        co_return pnm::EmptyResult<ITransport::Error>{};
    }

    pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> Bridge::receiveLoop(Lane lane, std::stop_token stop)
    {
        // This buffer lives in the coroutine frame across suspension and is
        // reused only after the previous frame has been dispatched.
        std::array<std::byte, util::MAX_FRAME_BYTES> buffer{};
        if (lane == Lane::Normal && !(co_await m_connection.waitReady(stop))) {
            co_return pnm::EmptyResult<ITransport::Error>{};
        }
        while (!stop.stop_requested()) {
            auto result = co_await m_transport.receive(buffer, lane, stop);
            if (!result) {
                if (result.error() == ITransport::Error::Cancelled && stop.stop_requested()) {
                    break;
                }
                // Includes BufferTooSmall: return the failure instead of
                // repeatedly retrying the same frame with the same buffer.
                co_return std::unexpected(result.error());
            }
            if (stop.stop_requested()) {
                break;
            }

            if (*result > buffer.size()) {
                throw std::length_error("Transport returned a frame larger than the receive buffer");
            }

            dispatchIncomingFrame(std::span<const std::byte>(buffer).first(*result), lane);
            co_await YieldToContext{ .context = &m_context };
        }
        co_return pnm::EmptyResult<ITransport::Error>{};
    }
}

// NOLINTEND(readability-static-accessed-through-instance)
