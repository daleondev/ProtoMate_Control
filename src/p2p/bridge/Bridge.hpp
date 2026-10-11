#pragma once

#include "../transport/ITransport.hpp"
#include "ServiceBridge.hpp"
#include "ActionBridge.hpp"
#include "TopicBridge.hpp"

#include <array>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <variant>

namespace p2p
{
    // Topic, service and action forwarding with bounded pending work.
    // Owns lifecycle and scheduling; its helpers own routing.
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
        // Register before start(), with matching names and payload types on both
        // peers. Each outgoing topic retains only its latest pending value.
        // Payloads use pneumo serialization. Portable wire representations require
        // adapters with explicit field sizes and byte order.
        template<typename Catalog, typename Publisher>
            requires std::is_enum_v<Publisher>
        void registerTopics(Publisher localPublisher);

        // Catalog annotations identify the PROVIDER. Replies return automatically.
        // Register before start(), with matching names and Request/Response types
        // on both peers. Applications use bus.service<^^Services::member>()
        // to request/serve; registration neither calls a service nor waits for it.
        // The bridge owns its routes and deferred proxies until stop().
        // At most eight forwarded calls per lane and direction and eight queued/pending
        // calls per local proxy. Excess calls fail with ServiceError::Busy.
        // Requests are never coalesced. Correlation IDs are internal to the bridge.
        // Caller timeouts/cancellation use pneumo's existing request API; they
        // end the caller's wait, not necessarily the remote handler's execution.

        // Like registerTopics(), for a catalog of nonstatic
        // pnm::msg::Service<Request, Response> fields. Annotate each with its
        // provider and optionally a FixedString name, using an application-owned enum
        // with two distinct values. Fields without that provider annotation stay
        // local; annotation order is irrelevant and unrelated annotations are ignored.
        // localProvider identifies this side: matching services accept
        // incoming requests; services provided by the peer forward outgoing ones.
        // For ServiceProvider { IO, UI }, pass IO on the IO bridge and UI on the
        // UI bridge. [[= ServiceProvider::IO]] marks a service provided by IO.
        // The application owns and dispatches local providers. For peer-provided
        // services, the bridge installs deferred proxies: do not also serve those
        // names locally. An optional [[= Lane::Urgent]] annotation selects reserved
        // service capacity and urgent request/reply traffic (for example Stop).
        // Both peers must use the same annotation; the default is Lane::Normal.
        // Deferred replies and ServiceError results return to callers.
        template<typename Catalog, typename Provider>
            requires std::is_enum_v<Provider>
        void registerServices(Provider localProvider);

        // Acceptance, feedback and result return automatically. Register before
        // start(), with matching payload types on both peers.
        // Admission timeouts cover acceptance, not the duration of accepted work.
        // Cancellation requests cleanup; only the provider confirms Cancelled.

        // Like registerServices(), using pnm::msg::Action<Goal, Feedback, Result>
        // fields. The application-owned enum has exactly two distinct values.
        // Matching localProvider -> incoming goals; peer provider -> outgoing.
        // Names use the same optional FixedString / underscore-to-slash rules.
        // Fields without the provider annotation stay local.
        // The application owns local providers and executes work on its workers.
        // Peer-provided actions get deferred proxies: do not also serve those
        // names locally. Local goal acceptance waits for the peer's acceptance.
        // Eight goals per direction, plus bounded rejection/acknowledgement space.
        // Feedback is latest-only; results are retained until peer acknowledgement.
        template<typename Catalog, typename Provider>
            requires std::is_enum_v<Provider>
        void registerActions(Provider localProvider);

        // Schedule a sender and receiver for each lane independently. May be called only once, including
        // after failure. The context must keep running until stop() completes.
        void start();

        // Stop action admission and request cancellation while communication runs.
        // Wait up to grace for action completion/delivery, then force closure and
        // record a failure if cleanup could not be confirmed. Finally join all four loops.
        // Repeated calls are harmless; before start(), this is a no-op.
        // Call stop() and the destructor outside the context runner so the loops
        // can finish. Do not stop the context first.
        void stop(std::chrono::milliseconds grace = std::chrono::milliseconds{500});

        // The first recorded failure, if any. Cancellation requested by stop()
        // is normal shutdown; an expired action drain deadline is a failure.
        [[nodiscard]] std::optional<Failure> failure() const;

      private:
        using OutgoingFrame = util::OutgoingFrame;

        // Registration and start() are serialized by m_mutex.
        void checkRegistration() const;
        // Cancel the other loops on exit and report completion only after releasing
        // the loop's coroutine frame and its borrowed transport buffers.
        pnm::coro::Task<void> runLoop(pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> loop,
                                      bool& running);
        pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> sendLoop(Lane lane, std::stop_token stop);
        pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> receiveLoop(Lane lane, std::stop_token stop);
        // Rotate this lane's ready frames, suspending when all are idle.
        pnm::coro::Task<std::optional<OutgoingFrame>> nextOutgoingFrame(Lane lane, std::stop_token stop);
        std::optional<OutgoingFrame> takeFrame(Lane lane, std::size_t helper);
        void dispatchIncomingFrame(std::span<const std::byte> frame, Lane lane);

        pnm::coro::Context& m_context;
        ITransport& m_transport;
        // Construct cancellation and readiness before the helpers that borrow them.
        std::stop_source m_stopSource;
        std::shared_ptr<util::OutgoingWakeup> m_outgoingWakeup{ std::make_shared<util::OutgoingWakeup>() };
        TopicBridge m_topics;
        ServiceBridge m_services;
        ActionBridge m_actions;
        std::array<std::size_t, 2> m_nextHelper{};
        // Keep the last action ACK alive while the other sender checks shutdown.
        std::array<bool, 2> m_actionSending{};
        std::atomic_bool m_shutdownRequested{};

        // Never request cancellation under this lock: stop callbacks can run inline.
        mutable std::mutex m_mutex;
        std::condition_variable m_finished;
        bool m_started{};
        bool m_sendRunning{};
        bool m_receiveRunning{};
        bool m_urgentSendRunning{};
        bool m_urgentReceiveRunning{};
        [[nodiscard]] bool loopsStopped() const;
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

    template<typename Catalog, typename Provider>
        requires std::is_enum_v<Provider>
    void Bridge::registerActions(Provider localProvider)
    {
        std::scoped_lock lock(m_mutex);
        checkRegistration();
        m_actions.registerCatalog<Catalog>(localProvider);
    }
}
