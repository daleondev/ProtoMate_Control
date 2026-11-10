#include "Bridge.hpp"
#include "profiling/Trace.hpp"

#include <stdexcept>
#include <utility>

// Coroutine lowering accesses static promise hooks through the promise object.
// NOLINTBEGIN(readability-static-accessed-through-instance)
namespace icc
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

    bool Bridge::loopsStopped() const
    {
        return !m_sendRunning && !m_receiveRunning && !m_urgentSendRunning && !m_urgentReceiveRunning &&
               !m_backgroundSendRunning && !m_backgroundReceiveRunning && !m_healthRunning &&
               !m_cleanupRunning;
    }

    std::optional<Bridge::Failure> Bridge::failure() const
    {
        std::scoped_lock lock{ m_mutex };
        return m_failure;
    }

    void Bridge::setConnectionCallback(ConnectionCallback callback)
    {
        std::scoped_lock lock{ m_mutex };
        checkRegistration();
        m_connection.setCallback(std::move(callback));
    }

    ConnectionStatus Bridge::connectionStatus() const { return m_connection.status(); }

    std::optional<pnm::msg::SubscriptionStatistics> Bridge::topicStatistics(std::string_view name) const
    {
        return m_topics.statistics(name);
    }

    void Bridge::beginShutdown()
    {
        m_connection.transition(
          { .state = ConnectionState::Stopping,
            .reason = m_peerStopped ? ConnectionReason::PeerStopped : ConnectionReason::None });
        m_actions.requestStop();
    }

    void Bridge::recordFailure(Failure failure, ConnectionReason reason)
    {
        std::scoped_lock lock{ m_mutex };
        if (!m_failure) {
            m_failure = std::move(failure);
            m_failureReason = reason;
        }
    }

    pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> Bridge::healthLoop(std::stop_token stop)
    {
        m_connection.begin();
        m_outgoingWakeup->signal(Lane::Urgent).notify();
        while (!stop.stop_requested()) {
            if (m_shutdownRequested.load()) {
                beginShutdown();
            }
            if (m_connection.tick()) {
                m_outgoingWakeup->signal(Lane::Urgent).notify();
            }
            auto timer{ m_context.scheduleAt(m_connection.nextWake(),
                                             [signal = m_healthWakeup] { signal->notify(); }) };
            if (!(co_await m_healthWakeup->ready.wait(stop))) {
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

    bool Bridge::nextOutgoingFrame(Lane lane, OutgoingFrame& outgoing)
    {
        if (m_shutdownRequested.load()) {
            beginShutdown();
        }
        m_connection.checkDeadline();
        if (lane != Lane::Background && m_connection.ready()) {
            m_services.poll();
            m_actions.poll();
        }
        if (m_shutdownRequested.load() && m_actions.drained() && !m_actionSending[0] && !m_actionSending[1]) {
            if (m_connection.goodbyeSent()) {
                m_stopSource.request_stop();
                return false;
            }
            m_connection.requestGoodbye();
            m_outgoingWakeup->signal(Lane::Urgent).notify();
        }
        if (auto frame{ takeReadyFrame(lane) }) {
            outgoing = *frame;
            return true;
        }
        return false;
    }

    std::optional<Bridge::OutgoingFrame> Bridge::takeReadyFrame(Lane lane)
    {
        if (lane == Lane::Background) {
            // Logs never participate in service/action admission or graceful
            // result draining. Pending background traffic is expendable at stop.
            return m_connection.ready() && !m_shutdownRequested.load() ? m_topics.takeFrame(lane)
                                                                       : std::nullopt;
        }
        if (lane == Lane::Urgent) {
            if (auto frame{ m_connection.takeFrame() }) {
                m_healthWakeup->notify();
                return frame;
            }
        }
        if (m_connection.ready()) {
            for (unsigned checked{ 0 }; checked < 3; ++checked) {
                if (auto frame{ takeFrame(lane, m_nextHelper[static_cast<std::size_t>(lane)]++ % 3) }) {
                    const auto kind{ static_cast<util::FrameKind>(frame->bytes[1]) };
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
                return lane == Lane::Normal ? m_topics.takeFrame(lane) : std::nullopt;
        }
    }

    void Bridge::dispatchIncomingFrame(std::span<const std::byte> bytes, Lane lane)
    {
        profiling::Scope measurement{ PROFILE_SITE_ICC_DISPATCH };
        m_connection.checkDeadline();
        const auto frame{ util::decode_frame(bytes) };
        profile_event(PROFILE_CAT_SYSTEM,
                      PROFILE_ICC_FRAME,
                      unsigned(frame.kind) | (unsigned(lane) << 8),
                      std::uint32_t(frame.id),
                      std::uint32_t(frame.id >> 32),
                      1);
        if (lane == Lane::Background &&
            (frame.kind != util::FrameKind::Topic || bytes.size() > MAX_BACKGROUND_FRAME_BYTES)) {
            throw std::invalid_argument("Invalid background frame");
        }
        if (frame.kind >= util::FrameKind::Hello) {
            const bool was_ready{ m_connection.ready() };
            if (m_connection.dispatch(frame, lane)) {
                m_peerStopped = true;
                if (!m_actions.drained()) {
                    throw util::ConnectionFailure(ConnectionReason::PeerStopped,
                                                  "Peer stopped with outstanding actions");
                }
                m_connection.transition(
                  { .state = ConnectionState::Stopping, .reason = ConnectionReason::PeerStopped });
                m_stopSource.request_stop();
            }
            m_healthWakeup->notify();
            if (!was_ready && m_connection.ready()) {
                // The handshake releases every application's outgoing queue.
                m_outgoingWakeup->notify();
            }
            else if (frame.kind == util::FrameKind::Hello || frame.kind == util::FrameKind::Heartbeat) {
                // Only the urgent sender has a reply to produce. Heartbeat
                // acknowledgements just reschedule the health timer.
                m_outgoingWakeup->signal(Lane::Urgent).notify();
            }
            return;
        }
        if (!m_connection.ready()) {
            throw std::invalid_argument("Application frame before handshake completion");
        }
        if (frame.kind == util::FrameKind::Topic) {
            if (lane == Lane::Urgent) {
                throw std::invalid_argument("Topics cannot use the urgent lane");
            }
            m_topics.dispatch(frame, lane);
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
            auto result{ co_await loop };
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
            std::scoped_lock lock{ m_mutex };
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
            std::scoped_lock lock{ m_mutex };
            m_cleanupRunning = false;
            m_finished.notify_all();
        }
    }

    void Bridge::finishConnection()
    {
        try {
            const bool failed{ failure().has_value() };
            m_services.close(failed ? pnm::msg::ServiceError::TransportError
                                    : pnm::msg::ServiceError::Cancelled);
            m_actions.close(failed ? pnm::msg::ActionError::TransportError
                                   : pnm::msg::ActionError::Unavailable);
            m_topics.close();
            ConnectionStatus status{};
            {
                std::scoped_lock lock{ m_mutex };
                status = m_failure
                           ? ConnectionStatus{ .state = ConnectionState::Failed, .reason = m_failureReason }
                           : ConnectionStatus{ .state = ConnectionState::Stopped,
                                               .reason = m_peerStopped ? ConnectionReason::PeerStopped
                                                                       : ConnectionReason::None };
            }
            m_connection.transition(status);
        } catch (...) {
            recordFailure(std::current_exception(), ConnectionReason::InternalError);
            ConnectionReason reason{};
            {
                std::scoped_lock lock{ m_mutex };
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
        // One frame per sender, allocated with the long-lived loop at startup.
        // A Task<optional<OutgoingFrame>> helper used to allocate another 4 KiB
        // on every message, including heartbeats, exhausting M4's motion budget.
        OutgoingFrame frame{};
        while (!stop.stop_requested()) {
            if (!nextOutgoingFrame(lane, frame)) {
                if (stop.stop_requested()) {
                    break;
                }
                if (!(co_await m_outgoingWakeup->signal(lane).ready.wait(stop))) {
                    if (stop.stop_requested()) {
                        break;
                    }
                    throw std::runtime_error("Bridge outgoing notification failed");
                }
                // Rescan after clearing the signal. A publication racing this
                // clear remains queued or supplies the next wakeup.
                m_outgoingWakeup->signal(lane).pending.clear();
                continue;
            }
            if (stop.stop_requested()) {
                break;
            }
            if (frame.size > frame.bytes.size()) {
                throw std::length_error("Outgoing bridge frame exceeds its buffer");
            }

            // frame owns the storage throughout the await. A full transport
            // suspends this loop while the receive loop can continue running.
            const auto bytes{ std::span<const std::byte>(frame.bytes).first(frame.size) };
#if defined(PROTOMATE_PROFILING)
            std::uint64_t trace_id{};
            if (bytes.size() >= util::HEADER_BYTES) {
                for (unsigned i{ 0 }; i < 8; ++i) {
                    trace_id |= std::uint64_t(std::to_integer<unsigned>(bytes[4 + i])) << (i * 8);
                }
            }
            profile_event(PROFILE_CAT_SYSTEM,
                          PROFILE_ICC_FRAME,
                          std::to_integer<unsigned>(bytes[1]) | (unsigned(lane) << 8),
                          std::uint32_t(trace_id),
                          std::uint32_t(trace_id >> 32),
                          0);
            profiling::Scope send_measurement{ PROFILE_SITE_ICC_SEND };
#endif
            auto result{ co_await m_transport.send(bytes, lane, stop) };
            if (!result) {
                if (result.error() == ITransport::Error::Cancelled && stop.stop_requested()) {
                    break;
                }
                co_return std::unexpected(result.error());
            }
            m_connection.sent(static_cast<util::FrameKind>(frame.bytes[1]));
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
        if (lane != Lane::Urgent && !(co_await m_connection.waitReady(stop))) {
            co_return pnm::EmptyResult<ITransport::Error>{};
        }
        while (!stop.stop_requested()) {
            auto result{ co_await m_transport.receive(buffer, lane, stop) };
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
