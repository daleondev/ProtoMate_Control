#include "p2p/transport/InterCoreTransport.hpp"
#include "p2p/bridge/Bridge.hpp"
#include "p2p/examples/ContextThread.hpp"
#include "hal/util/InterCoreMemory.hpp"
#include "common/Topics.hpp"

#include <gtest/gtest.h>
#include <future>
#include <limits>

namespace
{
    using namespace std::chrono_literals;
    using Channel = hal::IInterCoreChannel;
    using Queue = hal::util::InterCoreQueue;
    using Error = p2p::ITransport::Error;

    // Model only the MMIO/interrupt boundary, using the production ring layout.
    class ChannelModel final : public Channel
    {
      public:
        struct Pair
        {
            std::mutex mutex;
            hal::util::InterCoreMemory memory{};
            std::array<Notification, 2> notify;
            bool closed{};
        };
        ChannelModel(std::shared_ptr<Pair> pair, std::size_t side) : m_pair(std::move(pair)), m_side(side) {}
        std::expected<void, Channel::Error> trySend(std::span<const std::byte> bytes, Lane lane) noexcept override
        {
            const std::scoped_lock lock(m_pair->mutex);
            if (m_pair->closed) { return std::unexpected(Channel::Error::Disconnected); }
            auto result = m_pair->memory.inbox[1 - m_side][static_cast<std::size_t>(lane)].push(bytes);
            if (result && m_pair->notify[1 - m_side]) { m_pair->notify[1 - m_side](); }
            return result;
        }
        std::expected<std::size_t, Channel::Error> tryReceive(std::span<std::byte> bytes, Lane lane) noexcept override
        {
            const std::scoped_lock lock(m_pair->mutex);
            if (m_pair->closed) { return std::unexpected(Channel::Error::Disconnected); }
            auto result = m_pair->memory.inbox[m_side][static_cast<std::size_t>(lane)].pop(bytes);
            if (result && m_pair->notify[1 - m_side]) { m_pair->notify[1 - m_side](); }
            return result;
        }
        void setNotification(Notification callback) noexcept override
        {
            const std::scoped_lock lock(m_pair->mutex);
            m_pair->notify[m_side] = std::move(callback);
            if (m_pair->notify[m_side]) { m_pair->notify[m_side](); }
        }
        void close() noexcept override
        {
            const std::scoped_lock lock(m_pair->mutex);
            m_pair->closed = true;
            for (auto& callback : m_pair->notify) { if (callback) { callback(); } }
        }
      private:
        std::shared_ptr<Pair> m_pair;
        std::size_t m_side;
    };

    template<typename T>
    pnm::coro::Task<void> finish(pnm::coro::Task<T> task, std::shared_ptr<std::promise<T>> promise)
    {
        try { promise->set_value(co_await task); }
        catch (...) { promise->set_exception(std::current_exception()); }
    }

    struct TransportPair
    {
        std::shared_ptr<ChannelModel::Pair> memory{std::make_shared<ChannelModel::Pair>()};
        p2p::InterCoreTransport a{std::make_shared<ChannelModel>(memory, 0)};
        p2p::InterCoreTransport b{std::make_shared<ChannelModel>(memory, 1)};
        pnm::coro::Context context;
        std::jthread runner{example::run_context(context)};
        template<typename T>
        auto launch(pnm::coro::Task<T> task)
        {
            auto promise = std::make_shared<std::promise<T>>();
            auto future = promise->get_future();
            pnm::coro::co_spawn(context, finish(std::move(task), std::move(promise)));
            return future;
        }
        template<typename T>
        T run(pnm::coro::Task<T> task)
        {
            auto result = launch(std::move(task));
            if (result.wait_for(2s) != std::future_status::ready) { throw std::runtime_error("transport timeout"); }
            return result.get();
        }
    };

    TEST(InterCoreQueue, CounterRolloverCapacityAndNonDestructiveBufferErrors)
    {
        auto queue = std::make_unique<Queue>();
        queue->read = queue->written = std::numeric_limits<std::uint32_t>::max() - 1;
        const std::array frame{std::byte{1}, std::byte{2}};
        for (unsigned i{}; i < Queue::CAPACITY; ++i) { ASSERT_TRUE(queue->push(frame)); }
        EXPECT_EQ(queue->push(frame).error(), Channel::Error::WouldBlock);
        std::array<std::byte, 1> short_buffer{std::byte{9}};
        EXPECT_EQ(queue->pop(short_buffer).error(), Channel::Error::BufferTooSmall);
        EXPECT_EQ(short_buffer[0], std::byte{9});
        for (unsigned i{}; i < Queue::CAPACITY; ++i) {
            std::array<std::byte, 2> out{};
            ASSERT_EQ(queue->pop(out).value(), frame.size());
            EXPECT_EQ(out, frame);
        }
        EXPECT_EQ(queue->pop(short_buffer).error(), Channel::Error::WouldBlock);
        std::array<std::byte, Queue::MAX_FRAME_BYTES + 1> large{};
        EXPECT_EQ(queue->push(large).error(), Channel::Error::TooLarge);
    }

    TEST(InterCoreTransport, InterruptWakeupsPreserveFramesUnderBackpressure)
    {
        TransportPair pair;
        std::array frame{std::byte{42}};
        for (unsigned i{}; i < Queue::CAPACITY; ++i) { ASSERT_TRUE(pair.run(pair.a.send(frame))); }
        auto blocked = pair.launch(pair.a.send(frame));
        EXPECT_EQ(blocked.wait_for(10ms), std::future_status::timeout);
        std::array<std::byte, 1> out{};
        ASSERT_TRUE(pair.run(pair.b.receive(out)));
        ASSERT_EQ(blocked.wait_for(2s), std::future_status::ready);
        EXPECT_TRUE(blocked.get());
        for (unsigned i{}; i < Queue::CAPACITY; ++i) { ASSERT_TRUE(pair.run(pair.b.receive(out))); }
        // Waiting for data is woken even if the IRQ arrives before await starts.
        for (unsigned i{}; i < 100; ++i) {
            auto waiting = pair.launch(pair.b.receive(out));
            ASSERT_TRUE(pair.run(pair.a.send(frame)));
            ASSERT_EQ(waiting.wait_for(2s), std::future_status::ready);
            ASSERT_EQ(waiting.get().value(), 1U);
            EXPECT_EQ(out, frame);
        }
    }

    TEST(InterCoreQueue, ConcurrentProducerAndConsumerPublishCompletePayloads)
    {
        auto queue = std::make_unique<Queue>();
        constexpr std::uint32_t count{20'000};
        std::atomic_bool corrupt{};
        auto producer = std::thread([&] {
            for (std::uint32_t index{}; index < count; ++index) {
                std::array<std::uint32_t, 16> payload{};
                payload.fill(index);
                while (!queue->push(std::as_bytes(std::span(payload)))) { std::this_thread::yield(); }
            }
        });
        auto consumer = std::thread([&] {
            for (std::uint32_t index{}; index < count; ++index) {
                std::array<std::uint32_t, 16> payload{};
                while (!queue->pop(std::as_writable_bytes(std::span(payload)))) { std::this_thread::yield(); }
                for (auto value : payload) { if (value != index) { corrupt.store(true); } }
            }
        });
        producer.join();
        consumer.join();
        EXPECT_FALSE(corrupt.load());
        EXPECT_EQ(queue->read, count);
        EXPECT_EQ(queue->written, count);
    }

    TEST(InterCoreTransport, UrgentLaneAndCancellationDoNotWaitForNormalTraffic)
    {
        TransportPair pair;
        const std::array frame{std::byte{7}};
        for (unsigned i{}; i < Queue::CAPACITY; ++i) { ASSERT_TRUE(pair.run(pair.a.send(frame))); }
        std::stop_source stop;
        auto blocked = pair.launch(pair.a.send(frame, p2p::Lane::Normal, stop.get_token()));
        ASSERT_TRUE(pair.run(pair.a.send(frame, p2p::Lane::Urgent)));
        std::array<std::byte, 1> out{};
        ASSERT_TRUE(pair.run(pair.b.receive(out, p2p::Lane::Urgent)));
        EXPECT_EQ(out, frame);
        stop.request_stop();
        ASSERT_EQ(blocked.wait_for(2s), std::future_status::ready);
        EXPECT_EQ(blocked.get().error(), Error::Cancelled);
        auto receiver = pair.launch(pair.a.receive(out));
        pair.b.close();
        ASSERT_EQ(receiver.wait_for(2s), std::future_status::ready);
        EXPECT_EQ(receiver.get().error(), Error::Disconnected);
    }

    TEST(InterCoreTransport, CompleteBridgeHandshakeAndSerializedLedTopic)
    {
        TransportPair pair;
        pnm::msg::Bus io, ui;
        p2p::Bridge io_bridge{io, pair.context, pair.a};
        p2p::Bridge ui_bridge{ui, pair.context, pair.b};
        io_bridge.registerTopics<application::Topics>(application::TopicPublisher::IO);
        ui_bridge.registerTopics<application::Topics>(application::TopicPublisher::UI);
        application::LedState result;
        auto subscription = io.topic<^^application::Topics::io_led_state>().subscribe(
          [&](const application::LedState& state) { result = state; });
        io_bridge.start();
        ui_bridge.start();
        ui.topic<^^application::Topics::io_led_state>().publish(application::LedState{42, true});
        ASSERT_EQ(subscription.poll(2s), 1U);
        EXPECT_EQ(result.sequence, 42U);
        EXPECT_TRUE(result.on);
        ui_bridge.stop();
        io_bridge.stop();
    }
}
