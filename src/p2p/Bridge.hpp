#pragma once

#include "ITransport.hpp"

#include <pneumo/coroutines.hpp>
#include <pneumo/meta.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <meta>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace pnm::msg
{
    class Bus;
}

namespace p2p
{
    // Topic and service forwarding with bounded pending work.
    // Lifecycle, framing and dispatch live in Bridge.cpp; typed bus connections
    // are defined here so their serialization types remain visible to callers.
    class Bridge final
    {
      public:
        // Preserve either an ordinary transport failure or an unexpected exception.
        using Failure = std::variant<ITransport::Error, std::exception_ptr>;

        // The application owns these dependencies and keeps them alive until
        // the bridge has stopped and been destroyed.
        Bridge(pnm::msg::Bus& bus, pnm::coro::Context& context, ITransport& transport);
        ~Bridge();

        Bridge(const Bridge&) = delete;
        Bridge& operator=(const Bridge&) = delete;
        Bridge(Bridge&&) = delete;
        Bridge& operator=(Bridge&&) = delete;

        // Local topic -> encode -> transport -> peer.
        // Register before start(). The peer must call registerIncomingTopic<T>()
        // with the same topic name and compatible serialization for T.
        // Each topic retains one pending value; newer publications replace it.
        // Payloads use pneumo serialization. Portable wire representations require
        // adapters with explicit field sizes and byte order.
        template<pnm::utils::memory::Serializable T>
            requires std::default_initializable<T>
        void registerOutgoingTopic(std::string_view topic);

        // Peer -> transport -> decode -> publish on the local topic.
        // The transmitted topic name identifies the topic on both buses.
        template<pnm::utils::memory::Serializable T>
            requires std::default_initializable<T>
        void registerIncomingTopic(std::string_view topic);

        // Two-peer catalog: annotate public static constexpr TopicDefinition<T>
        // members with their publisher, using an application-owned enum.
        // localPublisher identifies this side: matching topics are outgoing;
        // topics published by the peer are incoming.
        // For TopicPublisher { IO, UI }, pass IO on the IO bridge and UI on the
        // UI bridge. Each call registers both local and remote publications.
        // The enum must declare exactly two distinct values. An invalid cast
        // passed as localPublisher is rejected before registering anything.
        // Unannotated members and annotations of other types are ignored.
        // Call before start(), just like the explicit registration functions.
        template<typename Catalog, typename Publisher>
            requires std::is_enum_v<Publisher>
        void registerTopics(Publisher localPublisher);

        // Explicit incoming/outgoing methods describe the REQUEST direction;
        // catalog annotations identify the PROVIDER. Replies return automatically.
        // Register before start(), with matching names and Request/Response types
        // on both peers. Applications still use bus.service<Request, Response>()
        // to request/serve; registration neither calls a service nor waits for it.
        // The bridge owns its routes and deferred proxies until stop().
        // At most eight forwarded calls per direction and eight queued/pending
        // calls per local proxy. Excess calls fail with ServiceError::Busy.
        // Requests are never coalesced. Correlation IDs are internal to the bridge.
        // Caller timeouts/cancellation use pneumo's existing request API; they
        // end the caller's wait, not necessarily the remote handler's execution.

        // Local request -> peer's provider -> response to the local caller.
        // Installs a deferred proxy on this bus: do not also serve this name
        // locally. The peer registers the corresponding incoming service.
        template<pnm::utils::memory::Serializable Request, pnm::utils::memory::Serializable Response>
            requires std::default_initializable<Request> && std::default_initializable<Response> &&
                     std::move_constructible<Response>
        void registerOutgoingService(std::string_view service);

        // Peer request -> local provider -> response to the peer.
        // The application owns and dispatches the provider on this bus; the bridge
        // submits requests and observes completion without blocking its context.
        // Deferred replies and ServiceError results travel back to the caller.
        template<pnm::utils::memory::Serializable Request, pnm::utils::memory::Serializable Response>
            requires std::default_initializable<Request> && std::default_initializable<Response> &&
                     std::move_constructible<Response>
        void registerIncomingService(std::string_view service);

        // Like registerTopics(), for a separate catalog of static constexpr
        // pnm::msg::ServiceDefinition<Request, Response> members. Annotate each
        // with its provider, using an application-owned enum with two distinct
        // values. localProvider identifies this side: matching services accept
        // incoming requests; services provided by the peer forward outgoing ones.
        // For ServiceProvider { IO, UI }, pass IO on the IO bridge and UI on the
        // UI bridge. [[= ServiceProvider::IO]] marks a service provided by IO.
        template<typename Catalog, typename Provider>
            requires std::is_enum_v<Provider>
        void registerServices(Provider localProvider);

        // Schedule both loops independently. May be called only once, including
        // after failure. The context must keep running until stop() completes.
        void start();

        // Request cancellation and wait for both loops to finish their cleanup.
        // Repeated calls are harmless; before start(), this is a no-op.
        // Call stop() and the destructor outside the context runner so the loops
        // can finish. Do not stop the context first.
        void stop();

        // The first recorded failure, if any. Cancellation requested by stop()
        // is normal shutdown and does not create a failure.
        [[nodiscard]] std::optional<Failure> failure() const;

      private:
        // v3: version u8, kind u8, name length u16, call ID u64, timeout ms u32,
        // name bytes, payload. All integer fields are little endian.
        enum class FrameKind : std::uint8_t
        {
            Topic = 1,
            Request,
            Response,
            Error,
            Cancel
        };
        using Clock = std::chrono::steady_clock;
        using ServiceError = pnm::msg::ServiceError;
        static constexpr std::uint8_t FRAME_VERSION{ 3 };
        static constexpr std::size_t HEADER_BYTES{ 16 };
        static constexpr std::size_t MAX_FRAME_BYTES{ 2048 };
        static constexpr std::size_t MAX_TOPICS{ 16 };
        static constexpr std::size_t MAX_SERVICES{ 16 };
        static constexpr std::size_t MAX_CALLS{ 8 };

        // Queueing must retain the bytes until send() completes. A fixed-size
        // value keeps that ownership explicit without a vector allocation.
        struct OutgoingFrame
        {
            std::array<std::byte, MAX_FRAME_BYTES> bytes{};
            std::size_t size{};
        };

        // Readiness callbacks may finish after unregistration. They retain only
        // this signal, never a pointer to Bridge. At most one wakeup is queued.
        struct OutgoingWakeup
        {
            pnm::coro::Channel<bool> ready;
            std::atomic_flag pending{};
            void notify() noexcept;
        };

        struct OutgoingTopic
        {
            std::string name;
            std::move_only_function<std::optional<OutgoingFrame>()> take;
            pnm::msg::ReadinessRegistration readiness;
        };

        struct IncomingTopic
        {
            std::string name;
            std::function<void(std::span<const std::byte>)> publish;
        };

        // These records are accessed only on the context thread after start().
        // Native readiness hooks and deadline timers only wake the sender.
        struct OutgoingService
        {
            std::string name;
            std::move_only_function<void()> poll;
            pnm::msg::ReadinessRegistration readiness;
        };

        struct OutgoingCall
        {
            std::uint64_t id{};
            std::optional<OutgoingFrame> request;
            std::function<bool()> pending;
            std::function<Clock::duration()> remaining;
            std::function<void(std::span<const std::byte>, std::optional<ServiceError>)> finish;
            pnm::msg::ReadinessRegistration readiness;
            pnm::coro::Context::Timer timer;
        };

        struct IncomingCall
        {
            std::uint64_t id{};
            std::move_only_function<std::optional<OutgoingFrame>()> take;
            std::move_only_function<void()> cancel;
            pnm::msg::ReadinessRegistration readiness;
            pnm::coro::Context::Timer timer;
        };

        struct IncomingService
        {
            std::string name;
            std::function<IncomingCall(std::uint64_t, std::span<const std::byte>, std::chrono::milliseconds)>
              request;
        };

        void checkServiceRegistration(std::string_view name) const;
        std::optional<OutgoingFrame> takeServiceFrame();
        void closeServices(ServiceError error);
        void queueServiceError(std::uint64_t id, ServiceError error);
        pnm::coro::Context::Timer wakeAfter(Clock::duration delay);
        static OutgoingFrame makeFrame(FrameKind kind,
                                       const std::string& name,
                                       std::uint64_t id,
                                       std::size_t payloadSize);
        static OutgoingFrame makeServiceError(std::uint64_t id, ServiceError error);

        template<pnm::utils::memory::Serializable T>
        static OutgoingFrame encodeFrame(FrameKind kind,
                                         const std::string& name,
                                         std::uint64_t id,
                                         const T& value);

        // Called during registration with m_mutex held. Topic names must
        // be unique across both directions, preventing immediate topic echo loops.
        void checkTopicRegistration(std::string_view topic) const;

        // Observe one loop, cancel its sibling on exit and signal completion
        // only after releasing the loop's coroutine frame and borrowed buffers.
        // running refers to one of this bridge's flags, protected by m_mutex.
        pnm::coro::Task<void> runLoop(pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> loop,
                                      bool& running);

        // Await a local outgoing frame, keeping its storage until send() completes.
        // Cancellation requested through stop is normal shutdown; other transport
        // failures are returned to runLoop().
        pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> sendLoop(std::stop_token stop);

        // Receive into a local buffer, then dispatch the returned bytes to the bus.
        pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> receiveLoop(std::stop_token stop);

        // Visit ready service calls and latest-only topic subscriptions fairly;
        // suspend on readiness when neither has a frame to send.
        // Cancellation returns nullopt; a value transfers its storage to sendLoop().
        pnm::coro::Task<std::optional<OutgoingFrame>> nextOutgoingFrame(std::stop_token stop);

        // Validate the frame and route its topic, request, reply or cancellation.
        // Must finish reading/copying the bytes before the receive buffer is reused.
        void dispatchIncomingFrame(std::span<const std::byte> frame);

        pnm::msg::Bus& m_bus;
        pnm::coro::Context& m_context;
        ITransport& m_transport;

        std::shared_ptr<OutgoingWakeup> m_outgoingWakeup{ std::make_shared<OutgoingWakeup>() };
        std::vector<OutgoingTopic> m_outgoingTopics;
        std::vector<IncomingTopic> m_incomingTopics;
        std::size_t m_nextOutgoingTopic{};
        std::vector<OutgoingService> m_outgoingServices;
        std::vector<IncomingService> m_incomingServices;
        std::vector<OutgoingCall> m_outgoingCalls;
        std::vector<IncomingCall> m_incomingCalls;
        std::deque<OutgoingFrame> m_serviceReplies;
        std::uint64_t m_lastCallId{};
        std::uint64_t m_lastIncomingCallId{};
        bool m_preferService{ true };
        ServiceError m_serviceShutdownError{ ServiceError::Cancelled };

        // Shared with the caller of start()/stop(); loop execution stays on the
        // context thread. Never request cancellation while holding this mutex:
        // stop callbacks can run synchronously.
        std::stop_source m_stopSource;
        mutable std::mutex m_mutex;
        std::condition_variable m_finished;
        bool m_started{};
        bool m_sendRunning{};
        bool m_receiveRunning{};
        std::optional<Failure> m_failure;
    };

    template<typename Catalog, typename Publisher>
        requires std::is_enum_v<Publisher>
    void Bridge::registerTopics(Publisher localPublisher)
    {
        static constexpr auto enumerators{ pnm::meta::enumeration::enumerators<Publisher>() };
        static_assert(enumerators.size() == 2, "A topic publisher enum must declare exactly two values");

        constexpr auto first{ enumerators[0] };
        constexpr auto second{ enumerators[1] };
        static_assert(std::to_underlying(first) != std::to_underlying(second),
                      "Topic publishers must have distinct values");

        if (localPublisher != first && localPublisher != second) {
            throw std::invalid_argument("Unknown local topic publisher");
        }

        static constexpr auto members{ std::define_static_array(
          std::meta::nonstatic_data_members_of(^^Catalog, std::meta::access_context::current())) };

        template for (constexpr auto member : members)
        {
            constexpr auto annotations{ std::define_static_array(std::meta::annotations_of(member)) };
            static_assert(annotations.size() == 2, "Topics need exactly two annotations.");

            // static_assert(std::meta::is_variable(annotations[0]) &&
            //                 std::meta::is_static_member(annotations[0]),
            //               "Annotate static constexpr topic definitions");

            using PublisherType = std::remove_cvref_t<typename[:std::meta::type_of(annotations[0]):]>;
            static_assert(std::same_as<PublisherType, Publisher>,
                          "The first annotation must be the publisher!");
            constexpr auto publisher{ std::meta::extract<Publisher>(annotations[0]) };

            using TopicType = std::remove_cvref_t<typename[:std::meta::type_of(annotations[1]):]>;
            static_assert(pnm::meta::string::FixedStringLike<TopicType>,
                          "The second annotation must be the topic!");
            constexpr auto topic{ std::meta::extract<TopicType>(annotations[1]) };

            using Message = std::remove_cvref_t<typename[:std::meta::type_of(member):]>;

            if (publisher == localPublisher)
                registerOutgoingTopic<Message>(topic);
            else
                registerIncomingTopic<Message>(topic);
        }
    }

    template<typename Catalog, typename Provider>
        requires std::is_enum_v<Provider>
    void Bridge::registerServices(Provider localProvider)
    {
        static constexpr auto enumerators{ std::define_static_array(std::meta::enumerators_of(^^Provider)) };
        static_assert(enumerators.size() == 2, "A service provider enum must declare exactly two values");
        constexpr auto first{ std::meta::extract<Provider>(enumerators[0]) };
        constexpr auto second{ std::meta::extract<Provider>(enumerators[1]) };
        static_assert(first != second, "Service providers must have distinct values");
        if (localProvider != first && localProvider != second)
            throw std::invalid_argument("Unknown local service provider");

        static constexpr auto members{ std::define_static_array(
          std::meta::members_of(^^Catalog, std::meta::access_context::current())) };

        template for (constexpr auto member : members)
        {
            constexpr auto providers{ std::define_static_array(
              std::meta::annotations_of_with_type(member, ^^Provider)) };
            if constexpr (!providers.empty()) {
                static_assert(providers.size() == 1, "A service must have exactly one provider");
                static_assert(std::meta::is_variable(member) && std::meta::is_static_member(member),
                              "Annotate static constexpr service definitions");

                constexpr auto service{[:member:] };
                using Definition = std::remove_cvref_t<decltype(service)>;
                using Request = typename Definition::Request;
                using Response = typename Definition::Response;
                static_assert(std::same_as<Definition, pnm::msg::ServiceDefinition<Request, Response>>,
                              "Annotated members must be ServiceDefinition<Request, Response> declarations");

                constexpr auto provider{ std::meta::extract<Provider>(providers[0]) };
                static_assert(provider == first || provider == second, "Unknown annotated service provider");
                if (provider == localProvider)
                    registerIncomingService<Request, Response>(service.name);
                else
                    registerOutgoingService<Request, Response>(service.name);
            }
        }
    }

    template<pnm::utils::memory::Serializable T>
        requires std::default_initializable<T>
    void Bridge::registerOutgoingTopic(std::string_view topic)
    {
        std::scoped_lock lock(m_mutex);
        checkTopicRegistration(topic);

        // The typed callback runs only when nextOutgoingFrame() polls it on the
        // context thread. Its result is moved straight into the send coroutine.
        auto pending = std::make_shared<std::optional<OutgoingFrame>>();
        auto subscription =
          m_bus.topic<T>(topic).subscribe([pending, name = std::string(topic)](const T& value) {
            *pending = encodeFrame(FrameKind::Topic, name, 0, value);
        }, pnm::msg::SubscriptionOptions::latestOnly());

        auto readiness = pnm::msg::watch(subscription, [signal = m_outgoingWakeup] { signal->notify(); });
        auto take = [subscription = std::move(subscription), pending]() mutable {
            subscription.poll(); // Nonblocking; latest-only means at most one callback.
            return std::exchange(*pending, std::nullopt);
        };
        m_outgoingTopics.push_back({
          .name = std::string(topic),
          .take = std::move(take),
          .readiness = std::move(readiness),
        });
    }

    template<pnm::utils::memory::Serializable T>
        requires std::default_initializable<T>
    void Bridge::registerIncomingTopic(std::string_view topic)
    {
        std::scoped_lock lock(m_mutex);
        checkTopicRegistration(topic);

        auto publish = [localTopic = m_bus.topic<T>(topic)](std::span<const std::byte> payload) {
            T value{};
            if (!pnm::utils::memory::deserialize(payload, value))
                throw std::invalid_argument("Invalid incoming topic payload");
            localTopic.publish(value);
        };
        m_incomingTopics.push_back({
          .name = std::string(topic),
          .publish = std::move(publish),
        });
    }
    template<pnm::utils::memory::Serializable T>
    Bridge::OutgoingFrame Bridge::encodeFrame(FrameKind kind,
                                              const std::string& name,
                                              std::uint64_t id,
                                              const T& value)
    {
        std::size_t size{ sizeof(T) };
        if constexpr (pnm::utils::memory::HasSerializationAdapter<T>)
            size = pnm::utils::memory::SerializationAdapter<T>::bufferSize(value);
        auto frame = makeFrame(kind, name, id, size);
        if (!pnm::utils::memory::serialize(value, std::span(frame.bytes).subspan(frame.size - size, size)))
            throw std::length_error("Cannot serialize bridge payload");
        return frame;
    }

    template<pnm::utils::memory::Serializable Request, pnm::utils::memory::Serializable Response>
        requires std::default_initializable<Request> && std::default_initializable<Response> &&
                 std::move_constructible<Response>
    void Bridge::registerOutgoingService(std::string_view service)
    {
        std::scoped_lock lock(m_mutex);
        checkServiceRegistration(service);
        auto proxy = m_bus.service<Request, Response>(service).serveDeferred(
          [this, name = std::string(service)](const Request& value, pnm::msg::Reply<Response> reply) {
            if (m_stopSource.stop_requested()) {
                reply.fail(m_serviceShutdownError);
                return;
            }
            if (m_outgoingCalls.size() >= MAX_CALLS) {
                reply.fail(ServiceError::Busy);
                return;
            }
            // Call IDs never wrap or get reused, including after cancellation.
            if (m_lastCallId == std::numeric_limits<std::uint64_t>::max()) {
                reply.fail(ServiceError::TransportError);
                return;
            }
            auto owned = std::make_shared<pnm::msg::Reply<Response>>(std::move(reply));
            try {
                const auto id = ++m_lastCallId;
                auto frame = encodeFrame(FrameKind::Request, name, id, value);
                auto timer = wakeAfter(owned->remainingTime());
                auto readiness = pnm::msg::watch(*owned, [signal = m_outgoingWakeup] { signal->notify(); });
                m_outgoingCalls.push_back({
                  .id = id,
                  .request = std::move(frame),
                  .pending = [owned] { return owned->pending(); },
                  .remaining = [owned] { return owned->remainingTime(); },
                  .finish =
                    [owned](std::span<const std::byte> bytes, std::optional<ServiceError> error) {
                    if (error) {
                        owned->fail(*error);
                        return;
                    }
                    try {
                        Response response{};
                        if (!pnm::utils::memory::deserialize(bytes, response))
                            throw std::invalid_argument("Invalid service response");
                        owned->respond(response);
                    } catch (...) {
                        owned->fail(ServiceError::TransportError);
                    }
                },
                  .readiness = std::move(readiness),
                  .timer = std::move(timer),
                });
            } catch (...) {
                // Oversized/unserializable requests never go to the peer.
                owned->fail(ServiceError::TransportError);
            }
        }, { .max_pending = MAX_CALLS });
        auto readiness = pnm::msg::watch(proxy, [signal = m_outgoingWakeup] { signal->notify(); });
        m_outgoingServices.push_back({
          .name = std::string(service),
          .poll = [proxy = std::move(proxy)]() mutable { proxy.poll(); },
          .readiness = std::move(readiness),
        });
    }

    template<pnm::utils::memory::Serializable Request, pnm::utils::memory::Serializable Response>
        requires std::default_initializable<Request> && std::default_initializable<Response> &&
                 std::move_constructible<Response>
    void Bridge::registerIncomingService(std::string_view service)
    {
        std::scoped_lock lock(m_mutex);
        checkServiceRegistration(service);
        auto request =
          [this, local = m_bus.service<Request, Response>(service)](
            std::uint64_t id, std::span<const std::byte> bytes, std::chrono::milliseconds timeout) {
            Request value{};
            if (!pnm::utils::memory::deserialize(bytes, value))
                throw std::invalid_argument("Invalid service request");
            // request() enqueues for the application's provider; it never executes
            // its handler here. Only inspect get() after ready() confirms completion.
            auto call = std::make_shared<pnm::msg::PendingCall<Response>>(local.request(value, timeout));
            auto readiness = pnm::msg::watch(*call, [signal = m_outgoingWakeup] { signal->notify(); });
            auto timer = wakeAfter(timeout);
            return IncomingCall{
                .id = id,
                .take = [call, id]() -> std::optional<OutgoingFrame> {
                if (!call->ready())
                    return std::nullopt;
                auto result = call->get();
                if (!result)
                    return makeServiceError(id, result.error());
                try {
                    return encodeFrame(FrameKind::Response, {}, id, *result);
                } catch (...) {
                    return makeServiceError(id, ServiceError::TransportError);
                }
            },
                .cancel = [call] { call->cancel(); },
                .readiness = std::move(readiness),
                .timer = std::move(timer),
            };
        };
        m_incomingServices.push_back({ .name = std::string(service), .request = std::move(request) });
    }
}
