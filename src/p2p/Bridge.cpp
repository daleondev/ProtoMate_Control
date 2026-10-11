#include "Bridge.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace p2p
{
    namespace
    {
        void write_integer(std::span<std::byte> bytes, std::uint64_t value)
        {
            for (auto& byte : bytes) {
                byte = static_cast<std::byte>(value & 0xFFU);
                value >>= 8U;
            }
        }

        std::uint64_t read_integer(std::span<const std::byte> bytes)
        {
            std::uint64_t value{};
            for (std::size_t i = 0; i < bytes.size(); ++i)
                value |= std::to_integer<std::uint64_t>(bytes[i]) << (8U * i);
            return value;
        }

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

    Bridge::Bridge(pnm::msg::Bus& bus, pnm::coro::Context& context, ITransport& transport)
      : m_bus(bus)
      , m_context(context)
      , m_transport(transport)
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
        m_outgoingTopics.clear();
        m_incomingTopics.clear();
        m_outgoingWakeup->ready.close();
    }

    std::optional<Bridge::Failure> Bridge::failure() const
    {
        std::scoped_lock lock(m_mutex);
        return m_failure;
    }

    void Bridge::OutgoingWakeup::notify() noexcept
    {
        if (pending.test_and_set())
            return;
        try {
            ready.push(true);
        } catch (...) {
            // Readiness callbacks cannot throw. Closing wakes the sender so it
            // can report a notification failure instead of silently waiting forever.
            ready.close();
        }
    }

    void Bridge::checkTopicRegistration(std::string_view topic) const
    {
        if (m_started)
            throw std::logic_error("Register bridge topics before start");
        if (topic.empty())
            throw std::invalid_argument("Bridge topics require a name");
        if (topic.size() > MAX_FRAME_BYTES - HEADER_BYTES)
            throw std::length_error("Topic name exceeds the bridge frame capacity");
        const auto duplicate = [topic](const auto& registered) { return registered.name == topic; };
        if (std::ranges::any_of(m_outgoingTopics, duplicate) ||
            std::ranges::any_of(m_incomingTopics, duplicate))
            throw std::invalid_argument("Bridge topic must be registered only once");
        if (m_outgoingTopics.size() + m_incomingTopics.size() >= MAX_TOPICS)
            throw std::length_error("Bridge topic capacity reached");
    }

    Bridge::OutgoingFrame Bridge::makeFrame(FrameKind kind, const std::string& name,
                                             std::uint64_t id, std::size_t payloadSize)
    {
        const auto nameSize = pnm::utils::memory::SerializationAdapter<std::string>::bufferSize(name);
        if (nameSize > MAX_FRAME_BYTES - HEADER_BYTES ||
            payloadSize > MAX_FRAME_BYTES - HEADER_BYTES - nameSize)
            throw std::length_error("Serialized message exceeds bridge frame capacity");
        OutgoingFrame frame;
        frame.size = HEADER_BYTES + nameSize + payloadSize;
        frame.bytes[0] = static_cast<std::byte>(FRAME_VERSION);
        frame.bytes[1] = static_cast<std::byte>(kind);
        write_integer(std::span(frame.bytes).subspan(2, 2), nameSize);
        write_integer(std::span(frame.bytes).subspan(4, 8), id);
        if (!pnm::utils::memory::serialize(name, std::span(frame.bytes).subspan(HEADER_BYTES, nameSize)))
            throw std::length_error("Cannot serialize bridge endpoint name");
        return frame;
    }

    Bridge::OutgoingFrame Bridge::makeServiceError(std::uint64_t id, ServiceError error)
    {
        auto frame = makeFrame(FrameKind::Error, {}, id, 1);
        frame.bytes[HEADER_BYTES] = encode_error(error);
        return frame;
    }

    void Bridge::checkServiceRegistration(std::string_view name) const
    {
        if (m_started)
            throw std::logic_error("Register bridge services before start");
        if (name.empty())
            throw std::invalid_argument("Bridge services require a name");
        if (name.size() > MAX_FRAME_BYTES - HEADER_BYTES)
            throw std::length_error("Service name exceeds bridge frame capacity");
        const auto duplicate = [name](const auto& route) { return route.name == name; };
        if (std::ranges::any_of(m_outgoingServices, duplicate) ||
            std::ranges::any_of(m_incomingServices, duplicate))
            throw std::invalid_argument("Bridge service must be registered only once");
        if (m_outgoingServices.size() + m_incomingServices.size() >= MAX_SERVICES)
            throw std::length_error("Bridge service capacity reached");
    }

    pnm::coro::Context::Timer Bridge::wakeAfter(Clock::duration delay)
    {
        const auto now = Clock::now();
        const auto deadline = delay >= Clock::time_point::max() - now
                                ? Clock::time_point::max() : now + delay;
        return m_context.scheduleAt(deadline, [signal = m_outgoingWakeup] { signal->notify(); });
    }

    void Bridge::queueServiceError(std::uint64_t id, ServiceError error)
    {
        // A conforming peer has at most MAX_CALLS outstanding. Excess rejection
        // traffic fails the connection instead of allocating an unbounded queue.
        if (m_serviceReplies.size() >= MAX_CALLS)
            throw std::runtime_error("Peer exceeded service reply capacity");
        m_serviceReplies.push_back(makeServiceError(id, error));
        m_outgoingWakeup->notify();
    }

    std::optional<Bridge::OutgoingFrame> Bridge::takeServiceFrame()
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
                return makeFrame(FrameKind::Cancel, {}, id, 0);
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
                write_integer(std::span(frame->bytes).subspan(12, 4), budget);
                return frame;
            }
        }
        return std::nullopt;
    }

    void Bridge::closeServices(ServiceError error)
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

    pnm::coro::Task<std::optional<Bridge::OutgoingFrame>> Bridge::nextOutgoingFrame(std::stop_token stop)
    {
        while (!stop.stop_requested()) {
            for (auto& service : m_outgoingServices)
                service.poll();
            if (m_preferService) {
                if (auto frame = takeServiceFrame()) {
                    m_preferService = false;
                    co_return frame;
                }
            }
            // Scan each bounded subscription once. Rotating the starting point
            // prevents one frequently published topic from starving the others.
            for (std::size_t offset = 0; offset < m_outgoingTopics.size(); ++offset) {
                const auto index = (m_nextOutgoingTopic + offset) % m_outgoingTopics.size();
                if (auto frame = m_outgoingTopics[index].take()) {
                    m_nextOutgoingTopic = (index + 1) % m_outgoingTopics.size();
                    m_preferService = true;
                    co_return frame;
                }
            }

            if (!m_preferService) {
                if (auto frame = takeServiceFrame())
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

    void Bridge::dispatchIncomingFrame(std::span<const std::byte> frame)
    {
        if (frame.size() < HEADER_BYTES || frame.size() > MAX_FRAME_BYTES)
            throw std::invalid_argument("Invalid bridge frame size");
        if (std::to_integer<std::uint8_t>(frame[0]) != FRAME_VERSION)
            throw std::invalid_argument("Unsupported bridge frame version");
        const auto kind = static_cast<FrameKind>(frame[1]);
        const auto nameSize = read_integer(frame.subspan(2, 2));
        const auto id = read_integer(frame.subspan(4, 8));
        const auto budget = read_integer(frame.subspan(12, 4));
        if (nameSize > frame.size() - HEADER_BYTES)
            throw std::invalid_argument("Invalid bridge name length");
        std::string name;
        if (!pnm::utils::memory::deserialize(frame.subspan(HEADER_BYTES, nameSize), name))
            throw std::invalid_argument("Invalid bridge name");
        const auto payload = frame.subspan(HEADER_BYTES + nameSize);
        if (kind == FrameKind::Topic) {
            if (name.empty() || id != 0 || budget != 0)
                throw std::invalid_argument("Invalid topic header");
            const auto topic = std::ranges::find(m_incomingTopics, name, &IncomingTopic::name);
            if (topic == m_incomingTopics.end())
                throw std::invalid_argument("Unknown incoming bridge topic");
            topic->publish(payload);
            return;
        }
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
            closeServices(m_failure ? ServiceError::TransportError : ServiceError::Cancelled);
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
        std::array<std::byte, MAX_FRAME_BYTES> buffer{};
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
