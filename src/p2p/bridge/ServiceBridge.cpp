#include "ServiceBridge.hpp"

#include <algorithm>

namespace p2p
{
    namespace
    {
        // Wire codes are explicit, independent of the native enum's layout.
        std::byte encode_error(pnm::msg::ServiceError error)
        {
            using enum pnm::msg::ServiceError;
            switch (error) {
            case Unavailable: return std::byte{1};
            case Busy: return std::byte{2};
            case Timeout: return std::byte{3};
            case Cancelled: return std::byte{4};
            case HandlerFailed: return std::byte{5};
            case TransportError: return std::byte{6};
            }
            throw std::invalid_argument("Unknown service error");
        }

        pnm::msg::ServiceError decode_error(std::byte error)
        {
            using enum pnm::msg::ServiceError;
            switch (std::to_integer<unsigned>(error)) {
            case 1: return Unavailable;
            case 2: return Busy;
            case 3: return Timeout;
            case 4: return Cancelled;
            case 5: return HandlerFailed;
            case 6: return TransportError;
            default: throw std::invalid_argument("Unknown wire service error");
            }
        }
    }

    ServiceBridge::ServiceBridge(pnm::msg::Bus& bus, pnm::coro::Context& context,
                                 std::shared_ptr<util::OutgoingWakeup> wakeup, std::stop_token stop)
      : m_bus(bus)
      , m_context(context)
      , m_outgoingWakeup(std::move(wakeup))
      , m_stop(stop)
    {
    }

    void ServiceBridge::poll()
    {
        for (auto& service : m_outgoingServices)
            service.poll();
    }

    ServiceBridge::OutgoingFrame ServiceBridge::makeServiceError(std::uint64_t id, ServiceError error)
    {
        auto frame = util::makeFrame(FrameKind::Error, {}, id, 1);
        frame.bytes[util::HEADER_BYTES] = encode_error(error);
        return frame;
    }

    void ServiceBridge::checkRegistration(std::string_view name) const
    {
        if (name.empty())
            throw std::invalid_argument("Bridge services require a name");
        if (name.size() > util::MAX_FRAME_BYTES - util::HEADER_BYTES)
            throw std::length_error("Service name exceeds bridge frame capacity");
        const auto duplicate = [name](const auto& route) { return route.name == name; };
        if (std::ranges::any_of(m_outgoingServices, duplicate) ||
            std::ranges::any_of(m_incomingServices, duplicate))
            throw std::invalid_argument("Bridge service must be registered only once");
        if (m_outgoingServices.size() + m_incomingServices.size() >= MAX_SERVICES)
            throw std::length_error("Bridge service capacity reached");
    }

    pnm::coro::Context::Timer ServiceBridge::wakeAfter(Clock::duration delay)
    {
        const auto now = Clock::now();
        const auto deadline = delay >= Clock::time_point::max() - now
                                ? Clock::time_point::max() : now + delay;
        return m_context.scheduleAt(deadline, [signal = m_outgoingWakeup] { signal->notify(); });
    }

    void ServiceBridge::queueServiceError(std::uint64_t id, ServiceError error)
    {
        // A conforming peer has at most MAX_CALLS outstanding. Excess rejection
        // traffic fails the connection instead of allocating an unbounded queue.
        if (m_serviceReplies.size() >= MAX_CALLS)
            throw std::runtime_error("Peer exceeded service reply capacity");
        m_serviceReplies.push_back(makeServiceError(id, error));
        m_outgoingWakeup->notify();
    }

    std::optional<ServiceBridge::OutgoingFrame> ServiceBridge::takeFrame()
    {
        // Cancel expired/abandoned calls first. A request already queued in the
        // transport cannot be recalled; cancellation is best effort at the peer.
        for (auto it = m_outgoingCalls.begin(); it != m_outgoingCalls.end();) {
            if (it->pending()) {
                ++it;
                continue;
            }
            const auto id = it->id;
            const auto wasSent = !it->request;
            it = m_outgoingCalls.erase(it);
            if (wasSent)
                return util::makeFrame(FrameKind::Cancel, {}, id, 0);
        }
        if (!m_serviceReplies.empty()) {
            auto frame = std::move(m_serviceReplies.front());
            m_serviceReplies.pop_front();
            return frame;
        }
        for (auto it = m_incomingCalls.begin(); it != m_incomingCalls.end(); ++it) {
            if (auto frame = it->take()) {
                m_incomingCalls.erase(it);
                return frame; // sendLoop owns the result until transport accepts it.
            }
        }
        for (auto& call : m_outgoingCalls) {
            if (call.request) {
                const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(call.remaining()).count();
                if (remaining <= 0) {
                    m_outgoingWakeup->notify();
                    continue;
                }
                auto frame = std::exchange(call.request, std::nullopt);
                const auto budget = std::min<std::uint64_t>(remaining, std::numeric_limits<std::uint32_t>::max());
                util::setRequestBudget(*frame, static_cast<std::uint32_t>(budget));
                return frame;
            }
        }
        return std::nullopt;
    }

    void ServiceBridge::close(ServiceError error)
    {
        m_serviceShutdownError = error;
        for (auto& call : m_outgoingCalls)
            call.finish({}, error);
        m_outgoingCalls.clear();
        for (auto& call : m_incomingCalls)
            call.cancel();
        m_incomingCalls.clear();
        // Dispatch queued proxy requests only to fail them, then remove proxies.
        // Application providers are not owned by the bridge and remain registered.
        for (auto& service : m_outgoingServices)
            service.poll();
        m_outgoingServices.clear();
        m_incomingServices.clear();
        m_serviceReplies.clear();
    }

    void ServiceBridge::dispatch(const util::IncomingFrame& frame)
    {
        const auto& [kind, name, id, budget, payload] = frame;
        if (id == 0)
            throw std::invalid_argument("Service call ID must be nonzero");
        if (kind == FrameKind::Request) {
            if (name.empty() || budget == 0 || id <= m_lastIncomingCallId)
                throw std::invalid_argument("Invalid or repeated service request");
            m_lastIncomingCallId = id;
            const auto service = std::ranges::find(m_incomingServices, name, &IncomingService::name);
            if (service == m_incomingServices.end()) {
                queueServiceError(id, ServiceError::Unavailable);
                return;
            }
            if (m_incomingCalls.size() >= MAX_CALLS) {
                queueServiceError(id, ServiceError::Busy);
                return;
            }
            try {
                m_incomingCalls.push_back(service->request(id, payload, std::chrono::milliseconds(budget)));
            } catch (...) {
                queueServiceError(id, ServiceError::HandlerFailed);
            }
            return;
        }
        if (!name.empty() || budget != 0)
            throw std::invalid_argument("Unexpected service reply/cancel header fields");
        if (kind == FrameKind::Cancel) {
            if (!payload.empty() || id > m_lastIncomingCallId)
                throw std::invalid_argument("Invalid service cancellation");
            const auto call = std::ranges::find(m_incomingCalls, id, &IncomingCall::id);
            if (call != m_incomingCalls.end()) {
                call->cancel();
                m_incomingCalls.erase(call);
            }
            return;
        }
        std::optional<ServiceError> error;
        if (kind == FrameKind::Error) {
            if (payload.size() != 1)
                throw std::invalid_argument("Invalid service error payload");
            error = decode_error(payload[0]);
        }
        else if (kind != FrameKind::Response)
            throw std::invalid_argument("Unknown bridge frame kind");
        if (id > m_lastCallId)
            throw std::invalid_argument("Reply for an unknown call ID");
        const auto call = std::ranges::find(m_outgoingCalls, id, &OutgoingCall::id);
        if (call != m_outgoingCalls.end()) {
            call->finish(payload, error);
            m_outgoingCalls.erase(call);
        }
        // Responses arriving after local timeout/cancellation are ignored.
    }
}
