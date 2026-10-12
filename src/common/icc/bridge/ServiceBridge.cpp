#include "ServiceBridge.hpp"

#include <algorithm>

namespace icc
{
    namespace
    {
        enum class WireError : std::uint8_t
        {
            Unavailable = 1,
            Busy = 2,
            Timeout = 3,
            Cancelled = 4,
            HandlerFailed = 5,
            TransportError = 6,
        };
        // Wire codes are explicit, independent of the native enum's layout.
        std::byte encode_error(pnm::msg::ServiceError error)
        {
            using enum pnm::msg::ServiceError;
            switch (error) {
                case Unavailable:
                    return static_cast<std::byte>(WireError::Unavailable);
                case Busy:
                    return static_cast<std::byte>(WireError::Busy);
                case Timeout:
                    return static_cast<std::byte>(WireError::Timeout);
                case Cancelled:
                    return static_cast<std::byte>(WireError::Cancelled);
                case HandlerFailed:
                    return static_cast<std::byte>(WireError::HandlerFailed);
                case TransportError:
                    return static_cast<std::byte>(WireError::TransportError);
            }
            throw std::invalid_argument("Unknown service error");
        }

        pnm::msg::ServiceError decode_error(std::byte error)
        {
            using enum pnm::msg::ServiceError;
            switch (static_cast<WireError>(error)) {
                case WireError::Unavailable:
                    return Unavailable;
                case WireError::Busy:
                    return Busy;
                case WireError::Timeout:
                    return Timeout;
                case WireError::Cancelled:
                    return Cancelled;
                case WireError::HandlerFailed:
                    return HandlerFailed;
                case WireError::TransportError:
                    return TransportError;
                default:
                    throw std::invalid_argument("Unknown wire service error");
            }
        }
    }

    ServiceBridge::ServiceBridge(pnm::msg::Bus& bus,
                                 pnm::coro::Context& context,
                                 std::shared_ptr<util::OutgoingWakeup> wakeup,
                                 std::stop_token stop)
      : m_bus(bus)
      , m_context(context)
      , m_outgoingWakeup(std::move(wakeup))
      , m_stop(std::move(stop))
    {
    }

    void ServiceBridge::poll()
    {
        for (auto& service : m_outgoingServices) {
            service.poll();
        }
    }

    ServiceBridge::OutgoingFrame ServiceBridge::makeServiceError(std::uint64_t id, ServiceError error)
    {
        auto frame = util::make_frame(FrameKind::Error, {}, id, 1);
        frame.bytes[util::HEADER_BYTES] = encode_error(error);
        return frame;
    }

    void ServiceBridge::checkRegistration(std::string_view name) const
    {
        if (name.empty()) {
            throw std::invalid_argument("Bridge services require a name");
        }
        if (name.size() > util::MAX_FRAME_BYTES - util::HEADER_BYTES) {
            throw std::length_error("Service name exceeds bridge frame capacity");
        }
        const auto duplicate = [name](const auto& route) { return route.name == name; };
        if (std::ranges::any_of(m_outgoingServices, duplicate) ||
            std::ranges::any_of(m_incomingServices, duplicate)) {
            throw std::invalid_argument("Bridge service must be registered only once");
        }
        if (m_outgoingServices.size() + m_incomingServices.size() >= MAX_SERVICES) {
            throw std::length_error("Bridge service capacity reached");
        }
    }

    pnm::coro::Context::Timer ServiceBridge::wakeAfter(Clock::duration delay)
    {
        const auto now = Clock::now();
        const auto deadline =
          delay >= Clock::time_point::max() - now ? Clock::time_point::max() : now + delay;
        return m_context.scheduleAt(deadline, [signal = m_outgoingWakeup] { signal->notify(); });
    }

    void ServiceBridge::queueServiceError(std::uint64_t id, ServiceError error, Lane lane)
    {
        // A conforming peer has at most MAX_CALLS outstanding. Excess rejection
        // traffic fails the connection instead of allocating an unbounded queue.
        if (std::ranges::count(m_serviceReplies, lane, &std::pair<Lane, OutgoingFrame>::first) >=
            static_cast<std::ptrdiff_t>(MAX_CALLS)) {
            throw std::runtime_error("Peer exceeded service reply capacity");
        }
        m_serviceReplies.emplace_back(lane, makeServiceError(id, error));
        m_outgoingWakeup->notify();
    }

    std::optional<ServiceBridge::OutgoingFrame> ServiceBridge::takeFrame(Lane lane)
    {
        // Cancel expired/abandoned calls first. A request already queued in the
        // transport cannot be recalled; cancellation is best effort at the peer.
        for (auto it = m_outgoingCalls.begin(); it != m_outgoingCalls.end();) {
            if (it->pending()) {
                ++it;
                continue;
            }
            if (it->request) {
                it = m_outgoingCalls.erase(it); // Never submitted to transport.
                continue;
            }
            // Keep its reserved slot until the peer responds, even after the
            // caller stops waiting. This also bounds overtaking cancellations.
            if (lane == Lane::Urgent && !it->cancel_sent) {
                it->cancel_sent = true;
                auto frame = util::make_frame(FrameKind::Cancel, {}, it->id, 1);
                frame.bytes[util::HEADER_BYTES] = it->lane == Lane::Urgent ? std::byte{ 1 } : std::byte{ 0 };
                return frame;
            }
            ++it;
        }
        const auto reply = std::ranges::find(m_serviceReplies, lane, &std::pair<Lane, OutgoingFrame>::first);
        if (reply != m_serviceReplies.end()) {
            auto frame = reply->second;
            m_serviceReplies.erase(reply);
            return frame;
        }
        for (auto it = m_incomingCalls.begin(); it != m_incomingCalls.end(); ++it) {
            if (it->lane != lane) {
                continue;
            }
            if (auto frame = it->take()) {
                m_incomingCalls.erase(it);
                return frame; // sendLoop owns the result until transport accepts it.
            }
        }
        for (auto& call : m_outgoingCalls) {
            if (call.lane == lane && call.request) {
                const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(call.remaining()).count();
                if (remaining <= 0) {
                    m_outgoingWakeup->notify();
                    continue;
                }
                auto frame = *call.request;
                call.request.reset();
                const auto budget =
                  std::min<std::uint64_t>(remaining, std::numeric_limits<std::uint32_t>::max());
                util::set_request_budget(frame, static_cast<std::uint32_t>(budget));
                return frame;
            }
        }
        return std::nullopt;
    }

    void ServiceBridge::close(ServiceError error)
    {
        m_serviceShutdownError = error;
        for (auto& call : m_outgoingCalls) {
            call.finish({}, error);
        }
        m_outgoingCalls.clear();
        for (auto& call : m_incomingCalls) {
            call.cancel();
        }
        m_incomingCalls.clear();
        // Dispatch queued proxy requests only to fail them, then remove proxies.
        // Application providers are not owned by the bridge and remain registered.
        for (auto& service : m_outgoingServices) {
            service.poll();
        }
        m_outgoingServices.clear();
        m_incomingServices.clear();
        m_serviceReplies.clear();
        m_earlyCancellations.clear();
    }

    void ServiceBridge::dispatch(const util::IncomingFrame& frame, Lane lane)
    {
        if (frame.id == 0) {
            throw std::invalid_argument("Service call ID must be nonzero");
        }
        if (frame.kind == FrameKind::Request) {
            dispatchRequest(frame, lane);
            return;
        }
        if (!frame.name.empty() || frame.budget != 0) {
            throw std::invalid_argument("Unexpected service reply/cancel header fields");
        }
        if (frame.kind == FrameKind::Cancel) {
            dispatchCancellation(frame, lane);
        }
        else {
            dispatchResponse(frame, lane);
        }
    }

    void ServiceBridge::dispatchRequest(const util::IncomingFrame& frame, Lane lane)
    {
        const auto& [kind, name, id, budget, payload] = frame;
        auto& last_incoming = m_lastIncomingCallId[static_cast<std::size_t>(lane)];
        if (name.empty() || budget == 0 || id <= last_incoming) {
            throw std::invalid_argument("Invalid or repeated service request");
        }
        last_incoming = id;
        const auto early =
          std::ranges::find(m_earlyCancellations, id, &std::pair<std::uint64_t, Lane>::first);
        if (early != m_earlyCancellations.end()) {
            if (early->second != lane) {
                throw std::invalid_argument("Service cancellation lane mismatch");
            }
            m_earlyCancellations.erase(early);
            queueServiceError(id, ServiceError::Cancelled, lane);
            return;
        }
        const auto service = std::ranges::find(m_incomingServices, name, &IncomingService::name);
        if (service == m_incomingServices.end()) {
            queueServiceError(id, ServiceError::Unavailable, lane);
            return;
        }
        if (service->lane != lane) {
            throw std::invalid_argument("Service request on the wrong transport lane");
        }
        if (std::ranges::count(m_incomingCalls, lane, &IncomingCall::lane) >=
            static_cast<std::ptrdiff_t>(MAX_CALLS)) {
            queueServiceError(id, ServiceError::Busy, lane);
            return;
        }
        try {
            m_incomingCalls.push_back(service->request(id, payload, std::chrono::milliseconds(budget)));
        } catch (...) {
            queueServiceError(id, ServiceError::HandlerFailed, lane);
        }
    }

    void ServiceBridge::dispatchCancellation(const util::IncomingFrame& frame, Lane lane)
    {
        const auto& [kind, name, id, budget, payload] = frame;
        if (lane != Lane::Urgent || payload.size() != 1 || std::to_integer<unsigned>(payload[0]) > 1) {
            throw std::invalid_argument("Invalid service cancellation");
        }
        const auto target_lane = payload[0] == std::byte{ 1 } ? Lane::Urgent : Lane::Normal;
        const auto call = std::ranges::find(m_incomingCalls, id, &IncomingCall::id);
        if (call != m_incomingCalls.end()) {
            if (call->lane != target_lane) {
                throw std::invalid_argument("Service cancellation lane mismatch");
            }
            call->cancel();
            m_incomingCalls.erase(call);
            queueServiceError(id, ServiceError::Cancelled, target_lane);
        }
        else if (id > m_lastIncomingCallId[static_cast<std::size_t>(target_lane)]) {
            const auto early =
              std::ranges::find(m_earlyCancellations, id, &std::pair<std::uint64_t, Lane>::first);
            if (early != m_earlyCancellations.end()) {
                if (early->second != target_lane) {
                    throw std::invalid_argument("Conflicting service cancellation lanes");
                }
            }
            else {
                if (std::ranges::count(
                      m_earlyCancellations, target_lane, &std::pair<std::uint64_t, Lane>::second) >=
                    static_cast<std::ptrdiff_t>(MAX_CALLS)) {
                    throw std::runtime_error("Peer exceeded early service cancellation capacity");
                }
                m_earlyCancellations.emplace_back(id, target_lane);
            }
        }
    }

    void ServiceBridge::dispatchResponse(const util::IncomingFrame& frame, Lane lane)
    {
        const auto& [kind, name, id, budget, payload] = frame;
        std::optional<ServiceError> error;
        if (kind == FrameKind::Error) {
            if (payload.size() != 1) {
                throw std::invalid_argument("Invalid service error payload");
            }
            error = decode_error(payload[0]);
        }
        else if (kind != FrameKind::Response) {
            throw std::invalid_argument("Unknown bridge frame kind");
        }
        if (id > m_lastCallId) {
            throw std::invalid_argument("Reply for an unknown call ID");
        }
        const auto call = std::ranges::find(m_outgoingCalls, id, &OutgoingCall::id);
        if (call != m_outgoingCalls.end()) {
            if (call->lane != lane) {
                throw std::invalid_argument("Service reply on the wrong transport lane");
            }
            call->finish(payload, error);
            m_outgoingCalls.erase(call);
        }
        // Responses arriving after local timeout/cancellation are ignored.
    }
}
