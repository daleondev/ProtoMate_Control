#include "ConnectionHealth.hpp"

#include <algorithm>
#include <limits>
#include <utility>

// Coroutine lowering accesses static promise hooks through the promise object.
// NOLINTBEGIN(readability-static-accessed-through-instance)
namespace p2p
{
    ConnectionHealth::ConnectionHealth(ConnectionOptions options)
      : m_options(options)
    {
        if (options.heartbeat <= std::chrono::milliseconds::zero() || options.timeout <= options.heartbeat ||
            options.timeout > std::chrono::days{ 1 }) {
            throw std::invalid_argument("Connection requires 0 < heartbeat < timeout <= 24 hours");
        }
    }

    ConnectionStatus ConnectionHealth::status() const { return m_status.load(); }

    void ConnectionHealth::setCallback(ConnectionCallback callback) { m_callback = std::move(callback); }

    void ConnectionHealth::transition(ConnectionStatus value)
    {
        if (m_status.exchange(value) == value) {
            return;
        }
        // The immutable registration and its captured state outlive stop().
        // Throwing callbacks fail the connection; Bridge protects final cleanup.
        if (m_callback) {
            try {
                m_callback(value);
            } catch (...) {
                std::throw_with_nested(
                  util::ConnectionFailure(ConnectionReason::InternalError, "Connection callback failed"));
            }
        }
    }

    void ConnectionHealth::begin()
    {
        m_deadline = Clock::now() + m_options.timeout;
        transition({ .state = ConnectionState::Connecting });
    }

    void ConnectionHealth::checkDeadline() const
    {
        if (m_deadline && Clock::now() >= *m_deadline) {
            throw util::ConnectionFailure(m_ready ? ConnectionReason::HeartbeatTimeout
                                                  : ConnectionReason::HandshakeTimeout,
                                          m_ready ? "Peer heartbeat expired" : "Peer handshake expired");
        }
    }

    bool ConnectionHealth::tick()
    {
        checkDeadline();
        if (m_nextHeartbeat && Clock::now() >= *m_nextHeartbeat) {
            m_nextHeartbeat.reset();
            return true;
        }
        return false;
    }

    ConnectionHealth::Clock::time_point ConnectionHealth::nextWake() const
    {
        // begin() arms the deadline before the health loop schedules its timer.
        if (!m_deadline) {
            throw std::logic_error("Connection timer has not started");
        }
        const auto deadline = *m_deadline;
        return std::min(deadline, m_nextHeartbeat.value_or(deadline));
    }

    std::optional<util::OutgoingFrame> ConnectionHealth::takeFrame()
    {
        if (!m_deadline) {
            return std::nullopt;
        }
        if (!std::exchange(m_helloSent, true)) {
            return util::make_frame(Kind::Hello, {}, 0, 0);
        }
        if (std::exchange(m_readyPending, false)) {
            return util::make_frame(Kind::Ready, {}, 0, 0);
        }
        if (std::exchange(m_goodbyePending, false)) {
            return util::make_frame(Kind::Goodbye, {}, 0, 0);
        }
        if (m_replyHeartbeat) {
            return util::make_frame(Kind::HeartbeatAck, {}, std::exchange(m_replyHeartbeat, 0), 0);
        }
        if (m_ready && !m_nextHeartbeat && !m_waitingAck) {
            if (m_lastHeartbeat == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("Heartbeat sequence exhausted");
            }
            m_waitingAck = true;
            return util::make_frame(Kind::Heartbeat, {}, ++m_lastHeartbeat, 0);
        }
        return std::nullopt;
    }

    bool ConnectionHealth::dispatch(const util::IncomingFrame& frame, Lane lane)
    {
        checkDeadline();
        if (lane != Lane::Urgent || !frame.name.empty() || frame.budget != 0 || !frame.payload.empty()) {
            throw std::invalid_argument("Invalid connection control frame");
        }
        const auto now = Clock::now();
        switch (frame.kind) {
            case Kind::Hello:
                if (frame.id != 0) {
                    throw std::invalid_argument("Invalid hello");
                }
                if (m_peerHello) {
                    throw util::ConnectionFailure(ConnectionReason::PeerRestarted,
                                                  "Peer restarted; explicit recovery required");
                }
                m_peerHello = true;
                m_readyPending = true;
                break;
            case Kind::Ready:
                if (frame.id != 0 || !m_peerHello || !m_helloSent || m_ready) {
                    throw std::invalid_argument("Unexpected peer readiness");
                }
                m_ready = true;
                m_deadline = now + m_options.timeout;
                m_nextHeartbeat = now + m_options.heartbeat;
                m_connected.close(); // Latch: wakes receivers without consuming a token.
                if (status().state != ConnectionState::Stopping) {
                    transition({ .state = ConnectionState::Connected });
                }
                break;
            case Kind::Heartbeat:
                if (!m_ready || frame.id == 0 || frame.id <= m_lastPeerHeartbeat) {
                    throw std::invalid_argument("Invalid heartbeat sequence");
                }
                m_lastPeerHeartbeat = frame.id;
                m_replyHeartbeat = frame.id;
                break;
            case Kind::HeartbeatAck:
                if (!m_ready || frame.id == 0 || frame.id > m_lastHeartbeat) {
                    throw std::invalid_argument("Invalid heartbeat acknowledgement");
                }
                // Only one challenge is outstanding. Replayed acknowledgements and
                // unrelated incoming traffic cannot keep a one-way connection alive.
                if (m_waitingAck && frame.id == m_lastHeartbeat) {
                    m_waitingAck = false;
                    m_deadline = now + m_options.timeout;
                    m_nextHeartbeat = now + m_options.heartbeat;
                }
                break;
            case Kind::Goodbye:
                if (frame.id != 0 || !m_peerHello) {
                    throw std::invalid_argument("Unexpected goodbye");
                }
                return true;
            default:
                throw std::invalid_argument("Unknown connection control frame");
        }
        return false;
    }

    void ConnectionHealth::requestGoodbye()
    {
        if (!m_goodbyeSent) {
            m_goodbyePending = true;
        }
    }
    void ConnectionHealth::sent(Kind kind)
    {
        if (kind == Kind::Goodbye) {
            m_goodbyeSent = true;
        }
    }

    pnm::coro::Task<bool> ConnectionHealth::waitReady(std::stop_token stop)
    {
        if (!m_ready) {
            co_await m_connected.next(stop);
        }
        co_return m_ready && !stop.stop_requested();
    }
}

// NOLINTEND(readability-static-accessed-through-instance)
