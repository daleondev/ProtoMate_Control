#pragma once

#include "ITransport.hpp"

#include <pneumo/coroutines.hpp>

#include <array>
#include <atomic>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace pnm::msg
{
    class Bus;
}

namespace p2p
{
    // Topic forwarding with bounded, latest-only outgoing subscriptions.
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
        // Frame: version (u8), topic byte length (u16 little endian), serialized
        // topic string, then payload. ITransport preserves the frame boundary.
        static constexpr std::uint8_t FRAME_VERSION{ 2 };
        static constexpr std::size_t HEADER_BYTES{ 3 };
        static constexpr std::size_t MAX_FRAME_BYTES{ 2048 };
        static constexpr std::size_t MAX_TOPICS{ 16 };

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

        // Called during registration with m_mutex held. Topic names must
        // be unique across both directions, preventing immediate topic echo loops.
        void checkTopicRegistration(std::string_view topic) const;
        static OutgoingFrame makeOutgoingFrame(const std::string& topic, std::size_t payloadSize);

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

        // The latest-only subscriptions are the bounded outgoing queue. Visit
        // them in turn, and suspend on readiness when none has a value to send.
        // Cancellation returns nullopt; a value transfers its storage to sendLoop().
        pnm::coro::Task<std::optional<OutgoingFrame>> nextOutgoingFrame(std::stop_token stop);

        // Validate the frame, decode its topic name and publish on the registered topic.
        // Must finish reading/copying the bytes before the receive buffer is reused.
        void dispatchIncomingFrame(std::span<const std::byte> frame);

        pnm::msg::Bus& m_bus;
        pnm::coro::Context& m_context;
        ITransport& m_transport;

        std::shared_ptr<OutgoingWakeup> m_outgoingWakeup{ std::make_shared<OutgoingWakeup>() };
        std::vector<OutgoingTopic> m_outgoingTopics;
        std::vector<IncomingTopic> m_incomingTopics;
        std::size_t m_nextOutgoingTopic{};

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
            std::size_t size{ sizeof(T) };
            if constexpr (pnm::utils::memory::HasSerializationAdapter<T>)
                size = pnm::utils::memory::SerializationAdapter<T>::bufferSize(value);

            auto frame = makeOutgoingFrame(name, size);
            auto payload = std::span<std::byte>(frame.bytes).subspan(frame.size - size, size);
            if (!pnm::utils::memory::serialize(value, payload))
                throw std::length_error("Cannot serialize outgoing topic into its frame");
            *pending = std::move(frame);
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
}
