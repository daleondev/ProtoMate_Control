#pragma once

#include <pneumo/coroutines.hpp>

#include <cstdint>
#include <stop_token>
#include <string_view>

namespace pnm::msg
{
    class Bus;
}

namespace p2p
{
    class ITransport;

    // First design step: forwarding topics between two independent local buses.
    // Declarations only. Transport, serialization and route storage follow later.
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

        // Start sendLoop() and receiveLoop() on the application-run context.
        void start();

        // Request cancellation and wait for both loops to finish their cleanup.
        // Call from outside the context runner so the loops can finish.
        void stop();

      private:
        // Await a local outgoing frame, keeping its storage until send() completes.
        pnm::coro::Task<void> sendLoop(std::stop_token stop);

        // Receive into a local buffer, then dispatch the returned bytes to the bus.
        pnm::coro::Task<void> receiveLoop(std::stop_token stop);

        pnm::msg::Bus& m_bus;
        pnm::coro::Context& m_context;
        ITransport& m_transport;
    };
}
