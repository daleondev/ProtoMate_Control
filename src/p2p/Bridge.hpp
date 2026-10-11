#pragma once

#include "ITransport.hpp"

#include <pneumo/coroutines.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>

namespace pnm::msg
{
    class Bus;
}

namespace p2p
{
    // First implementation step: the two transport loops in Bridge.cpp.
    // Lifecycle, queueing and topic registration remain declarations for now.
    class Bridge final
    {
      public:
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

        // Next step: start both loops on the application-run context and track
        // their completion. Lifecycle is not implemented yet.
        void start();

        // Request cancellation and wait for both loops to finish their cleanup.
        // Call from outside the context runner so the loops can finish.
        void stop();

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

        // Await a local outgoing frame, keeping its storage until send() completes.
        // Cancellation requested through stop is normal shutdown; other transport
        // failures are returned for the future lifecycle code to handle.
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
    };
}
