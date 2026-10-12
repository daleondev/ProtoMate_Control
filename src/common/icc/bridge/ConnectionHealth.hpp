#pragma once

#include "../transport/ITransport.hpp"
#include "Frame.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>

namespace icc
{
    enum class ConnectionState : std::uint8_t
    {
        Stopped,
        Connecting,
        Connected,
        Stopping,
        Failed
    };
    enum class ConnectionReason : std::uint8_t
    {
        None,
        PeerStopped,
        HandshakeTimeout,
        HeartbeatTimeout,
        PeerRestarted,
        TransportError,
        ProtocolError,
        ShutdownTimeout,
        InternalError
    };
    struct ConnectionStatus
    {
        ConnectionState state{ ConnectionState::Stopped };
        ConnectionReason reason{ ConnectionReason::None };
        bool operator==(const ConnectionStatus&) const = default;
    };
    // Require 0 < heartbeat < timeout <= 24 hours. Defaults suit the control link.
    struct ConnectionOptions
    {
        static constexpr std::chrono::milliseconds DEFAULT_HEARTBEAT{ 100 };
        static constexpr std::chrono::milliseconds DEFAULT_TIMEOUT{ 500 };
        std::chrono::milliseconds heartbeat{ DEFAULT_HEARTBEAT };
        std::chrono::milliseconds timeout{ DEFAULT_TIMEOUT };
    };
    using ConnectionCallback = std::function<void(ConnectionStatus)>;

    // Internal protocol failure carrying a stable, application-visible reason.
    namespace util
    {
        class ConnectionFailure : public std::runtime_error
        {
          public:
            ConnectionFailure(ConnectionReason reason, const char* message)
              : std::runtime_error(message)
              , reason(reason)
            {
            }
            ConnectionReason reason;
        };
    }

    // Context-owned handshake/heartbeat state. Only status() is read from other
    // threads; callbacks run on the context, outside the bridge lifecycle lock.
    class ConnectionHealth final
    {
        friend class Bridge;
        using Clock = std::chrono::steady_clock;
        using Kind = util::FrameKind;

        explicit ConnectionHealth(ConnectionOptions options);
        ConnectionStatus status() const;
        void setCallback(ConnectionCallback callback);
        void transition(ConnectionStatus value);
        void begin();
        void checkDeadline() const;
        bool tick();
        Clock::time_point nextWake() const;
        std::optional<util::OutgoingFrame> takeFrame();
        // True only for an orderly peer goodbye. Bridge owns execution cleanup.
        bool dispatch(const util::IncomingFrame& frame, Lane lane);
        void requestGoodbye();
        void sent(Kind kind);
        bool ready() const { return m_ready; }
        bool goodbyeSent() const { return m_goodbyeSent; }
        pnm::coro::Task<bool> waitReady(std::stop_token stop);

        const ConnectionOptions m_options;
        // Publish state and reason together without a mutex, including on STM32.
        static_assert(std::atomic<ConnectionStatus>::is_always_lock_free);
        std::atomic<ConnectionStatus> m_status;
        ConnectionCallback m_callback;
        pnm::coro::Channel<bool> m_connected;
        std::optional<Clock::time_point> m_deadline;
        // After readiness: a time schedules the next heartbeat; no time means
        // it is pending or already sent and waiting for acknowledgement.
        std::optional<Clock::time_point> m_nextHeartbeat;
        std::uint64_t m_lastHeartbeat{}, m_lastPeerHeartbeat{}, m_replyHeartbeat{};
        bool m_helloSent{}, m_peerHello{}, m_readyPending{}, m_ready{};
        bool m_waitingAck{}, m_goodbyePending{}, m_goodbyeSent{};
    };
}
