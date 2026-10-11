#pragma once

#include "ITransport.hpp"

#include <pneumo/coroutines.hpp>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <variant>

namespace pnm::msg
{
    class Bus;
}

namespace p2p
{
    // Lifecycle and transport loops live in Bridge.cpp.
    // Queueing and topic registration remain declarations for the next step.
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
        // with the same endpoint ID and compatible serialization for T.
        template<pnm::utils::memory::Serializable T>
        void registerOutgoingTopic(std::uint16_t endpoint, std::string_view localTopic);

        // Peer -> transport -> decode -> publish on the local topic.
        // The endpoint identifies the route across the connection; localTopic
        // identifies it on this bus. The two sides may use different local names.
        template<pnm::utils::memory::Serializable T>
        void registerIncomingTopic(std::uint16_t endpoint, std::string_view localTopic);

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
        // Initial full-frame capacity, including the future routing header.
        static constexpr std::size_t MAX_FRAME_BYTES{ 2048 };

        // Queueing must retain the bytes until send() completes. A fixed-size
        // value keeps that ownership explicit without a vector allocation.
        struct OutgoingFrame
        {
            std::array<std::byte, MAX_FRAME_BYTES> bytes{};
            std::size_t size{};
        };

        // Observe one loop, cancel its sibling on exit and signal completion
        // only after releasing the loop's coroutine frame and borrowed buffers.
        pnm::coro::Task<void> runLoop(pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> loop);

        // Await a local outgoing frame, keeping its storage until send() completes.
        // Cancellation requested through stop is normal shutdown; other transport
        // failures are returned to runLoop().
        pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> sendLoop(std::stop_token stop);

        // Receive into a local buffer, then dispatch the returned bytes to the bus.
        pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> receiveLoop(std::stop_token stop);

        // Next step: await a frame from the bounded outgoing queue. Cancellation
        // returns nullopt. Returning a value transfers its storage to sendLoop().
        pnm::coro::Task<std::optional<OutgoingFrame>> nextOutgoingFrame(std::stop_token stop);

        // Next step: decode the endpoint and publish through its registered route.
        // Must finish reading/copying the bytes before the receive buffer is reused.
        void dispatchIncomingFrame(std::span<const std::byte> frame);

        pnm::msg::Bus& m_bus;
        pnm::coro::Context& m_context;
        ITransport& m_transport;

        // Shared with the caller of start()/stop(); loop execution stays on the
        // context thread. Never request cancellation while holding this mutex:
        // stop callbacks can run synchronously.
        std::stop_source m_stopSource;
        mutable std::mutex m_mutex;
        std::condition_variable m_finished;
        bool m_started{};
        std::size_t m_activeLoops{};
        std::optional<Failure> m_failure;
    };
}
