#pragma once

#include "TopicBridge.hpp"
#include "ServiceBridge.hpp"
#include "../transport/ITransport.hpp"

#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <variant>

namespace p2p
{
    // Topic and service forwarding with bounded pending work.
    // Owns lifecycle and scheduling; TopicBridge and ServiceBridge own routing.
    // The public registration API delegates to those helpers under one lifecycle lock.
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

        // Two-peer catalog: annotate public nonstatic pnm::msg::Topic<T> fields
        // with a publisher from an application-owned enum. An optional FixedString
        // overrides the name; otherwise underscores in the field name become '/'.
        // Annotation order is irrelevant. No catalog instance is needed.
        // localPublisher identifies this side: matching topics are outgoing;
        // topics published by the peer are incoming.
        // For TopicPublisher { IO, UI }, pass IO on the IO bridge and UI on the
        // UI bridge. Each call registers both local and remote publications.
        // The enum must declare exactly two distinct values. An invalid cast
        // passed as localPublisher is rejected before registering anything.
        // Members without this publisher annotation stay local. Unrelated
        // annotations are ignored; annotated static declarations are rejected.
        // Call before start(), just like the explicit registration functions.
        template<typename Catalog, typename Publisher>
            requires std::is_enum_v<Publisher>
        void registerTopics(Publisher localPublisher);

        // Explicit incoming/outgoing methods describe the REQUEST direction;
        // catalog annotations identify the PROVIDER. Replies return automatically.
        // Register before start(), with matching names and Request/Response types
        // on both peers. Applications use bus.service<^^Services::member>()
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

        // Like registerTopics(), for a catalog of nonstatic
        // pnm::msg::Service<Request, Response> fields. Annotate each with its
        // provider and optionally a FixedString name, using an application-owned enum
        // with two distinct values. Fields without that provider annotation stay
        // local; annotation order is irrelevant and unrelated annotations are ignored.
        // localProvider identifies this side: matching services accept
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
        using OutgoingFrame = util::OutgoingFrame;

        // Registration and start() are serialized by m_mutex.
        void checkRegistration() const;
        // Cancel the sibling on exit and report completion only after releasing
        // the loop's coroutine frame and its borrowed transport buffers.
        pnm::coro::Task<void> runLoop(pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> loop,
                                      bool& running);
        pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> sendLoop(std::stop_token stop);
        pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> receiveLoop(std::stop_token stop);
        // Alternate ready topic/service frames, suspending when both are idle.
        pnm::coro::Task<std::optional<OutgoingFrame>> nextOutgoingFrame(std::stop_token stop);
        void dispatchIncomingFrame(std::span<const std::byte> frame);

        pnm::coro::Context& m_context;
        ITransport& m_transport;
        // Construct cancellation and readiness before the helpers that borrow them.
        std::stop_source m_stopSource;
        std::shared_ptr<util::OutgoingWakeup> m_outgoingWakeup{ std::make_shared<util::OutgoingWakeup>() };
        TopicBridge m_topics;
        ServiceBridge m_services;
        bool m_preferService{ true };

        // Never request cancellation under this lock: stop callbacks can run inline.
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
        std::scoped_lock lock(m_mutex);
        checkRegistration();
        m_topics.registerCatalog<Catalog>(localPublisher);
    }

    template<typename Catalog, typename Provider>
        requires std::is_enum_v<Provider>
    void Bridge::registerServices(Provider localProvider)
    {
        std::scoped_lock lock(m_mutex);
        checkRegistration();
        m_services.registerCatalog<Catalog>(localProvider);
    }

    template<pnm::utils::memory::Serializable T>
        requires std::default_initializable<T>
    void Bridge::registerOutgoingTopic(std::string_view topic)
    {
        std::scoped_lock lock(m_mutex);
        checkRegistration();
        m_topics.registerOutgoing<T>(topic);
    }

    template<pnm::utils::memory::Serializable T>
        requires std::default_initializable<T>
    void Bridge::registerIncomingTopic(std::string_view topic)
    {
        std::scoped_lock lock(m_mutex);
        checkRegistration();
        m_topics.registerIncoming<T>(topic);
    }

    template<pnm::utils::memory::Serializable Request, pnm::utils::memory::Serializable Response>
        requires std::default_initializable<Request> && std::default_initializable<Response> &&
                 std::move_constructible<Response>
    void Bridge::registerOutgoingService(std::string_view service)
    {
        std::scoped_lock lock(m_mutex);
        checkRegistration();
        m_services.registerOutgoing<Request, Response>(service);
    }

    template<pnm::utils::memory::Serializable Request, pnm::utils::memory::Serializable Response>
        requires std::default_initializable<Request> && std::default_initializable<Response> &&
                 std::move_constructible<Response>
    void Bridge::registerIncomingService(std::string_view service)
    {
        std::scoped_lock lock(m_mutex);
        checkRegistration();
        m_services.registerIncoming<Request, Response>(service);
    }
}
