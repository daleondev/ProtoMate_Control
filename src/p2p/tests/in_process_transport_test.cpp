#include "p2p/Bridge.hpp"
#include "p2p/InProcessTransport.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <span>
#include <stop_token>
#include <thread>
#include <utility>

namespace
{
    using namespace std::chrono_literals;
    using Transport = p2p::InProcessTransport;
    using Error = p2p::ITransport::Error;

    template<typename T>
    pnm::coro::Task<void> complete(pnm::coro::Task<T> task, std::shared_ptr<std::promise<T>> promise)
    {
        try {
            if constexpr (std::is_void_v<T>) {
                co_await task;
                task = {};
                promise->set_value();
            }
            else {
                auto result = co_await task;
                task = {};
                promise->set_value(std::move(result));
            }
        } catch (...) {
            task = {};
            promise->set_exception(std::current_exception());
        }
    }

    template<typename T>
    std::future<T> launch(pnm::coro::Context& context, pnm::coro::Task<T> task)
    {
        auto promise = std::make_shared<std::promise<T>>();
        auto result = promise->get_future();
        pnm::coro::co_spawn(context, complete(std::move(task), std::move(promise)));
        return result;
    }

    class TransportTest : public ::testing::Test
    {
      protected:
        pnm::coro::Context context;
        std::array<std::shared_ptr<Transport>, 2> endpoints{ Transport::pair() };

        void drain()
        {
            while (context.poll() != 0) {
            }
        }

        template<typename T>
        T run(pnm::coro::Task<T> task)
        {
            auto result = launch(context, std::move(task));
            drain();
            if (result.wait_for(0ms) != std::future_status::ready)
                throw std::runtime_error("Expected the operation to finish without waiting");
            return result.get();
        }

        void fill(std::size_t sender = 0)
        {
            for (std::size_t i = 0; i < Transport::FRAME_CAPACITY; ++i) {
                const std::array value{ static_cast<std::byte>(i) };
                ASSERT_TRUE(run(endpoints[sender]->send(value)));
            }
        }

        void TearDown() override
        {
            endpoints[0]->close();
            drain();
        }
    };

    TEST_F(TransportTest, CopiesFramesAndPreservesFifoAcrossRingWraps)
    {
        for (std::size_t round = 0; round < 3; ++round) {
            for (std::size_t i = 0; i < Transport::FRAME_CAPACITY; ++i) {
                std::array frame{ static_cast<std::byte>(round), static_cast<std::byte>(i) };
                ASSERT_TRUE(run(endpoints[0]->send(frame)));
                frame.fill(std::byte{ 0xFF }); // The queue owns a copy now.
            }
            for (std::size_t i = 0; i < Transport::FRAME_CAPACITY; ++i) {
                std::array<std::byte, 3> received{};
                auto result = run(endpoints[1]->receive(received));
                ASSERT_TRUE(result);
                EXPECT_EQ(*result, 2U);
                EXPECT_EQ(received[0], static_cast<std::byte>(round));
                EXPECT_EQ(received[1], static_cast<std::byte>(i));
                EXPECT_EQ(received[2], std::byte{});
            }
        }
    }

    TEST_F(TransportTest, SupportsEmptyAndMaximumFramesAndRejectsOversizedFrames)
    {
        ASSERT_TRUE(run(endpoints[0]->send({})));
        auto empty = run(endpoints[1]->receive({}));
        ASSERT_TRUE(empty);
        EXPECT_EQ(*empty, 0U);

        std::array<std::byte, Transport::MAX_FRAME_BYTES> frame;
        frame.fill(std::byte{ 0xAC });
        ASSERT_TRUE(run(endpoints[0]->send(frame)));
        std::array<std::byte, Transport::MAX_FRAME_BYTES> received{};
        auto result = run(endpoints[1]->receive(received));
        ASSERT_TRUE(result);
        EXPECT_EQ(*result, frame.size());
        EXPECT_EQ(received, frame);

        std::array<std::byte, Transport::MAX_FRAME_BYTES + 1> oversized{};
        EXPECT_EQ(run(endpoints[0]->send(oversized)), std::unexpected(Error::TooLarge));
        // Oversized input wasn't queued; an empty receive still waits.
        std::stop_source stop;
        auto waiting = launch(context, endpoints[1]->receive(received, stop.get_token()));
        drain();
        EXPECT_EQ(waiting.wait_for(0ms), std::future_status::timeout);
        stop.request_stop();
        drain();
        EXPECT_EQ(waiting.get(), std::unexpected(Error::Cancelled));
    }

    TEST_F(TransportTest, EmptyReceiveSuspendsAndWakesWhenDataArrives)
    {
        std::array<std::byte, 3> received{};
        auto waiting = launch(context, endpoints[1]->receive(received));
        drain();
        EXPECT_EQ(waiting.wait_for(0ms), std::future_status::timeout);
        const std::array frame{ std::byte{ 42 }, std::byte{ 17 } };
        ASSERT_TRUE(run(endpoints[0]->send(frame)));
        ASSERT_EQ(waiting.wait_for(0ms), std::future_status::ready);
        auto result = waiting.get();
        ASSERT_TRUE(result);
        EXPECT_EQ(*result, frame.size());
        EXPECT_TRUE(std::ranges::equal(frame, std::span(received).first(frame.size())));
        EXPECT_EQ(received.back(), std::byte{});
    }

    TEST_F(TransportTest, FullSendSuspendsWhileReverseDirectionRemainsUsable)
    {
        fill();
        const std::array extra{ std::byte{ 99 } };
        auto blocked = launch(context, endpoints[0]->send(extra));
        drain();
        EXPECT_EQ(blocked.wait_for(0ms), std::future_status::timeout);

        ASSERT_TRUE(run(endpoints[1]->send(extra)));
        std::array<std::byte, 1> received{};
        ASSERT_TRUE(run(endpoints[0]->receive(received)));
        EXPECT_EQ(received, extra);
        EXPECT_EQ(blocked.wait_for(0ms), std::future_status::timeout);

        ASSERT_TRUE(run(endpoints[1]->receive(received)));
        EXPECT_EQ(received[0], std::byte{ 0 });
        ASSERT_EQ(blocked.wait_for(0ms), std::future_status::ready);
        ASSERT_TRUE(blocked.get());
        for (std::size_t i = 1; i < Transport::FRAME_CAPACITY; ++i) {
            ASSERT_TRUE(run(endpoints[1]->receive(received)));
            EXPECT_EQ(received[0], static_cast<std::byte>(i));
        }
        ASSERT_TRUE(run(endpoints[1]->receive(received)));
        EXPECT_EQ(received, extra);
    }

    TEST_F(TransportTest, SmallReceiveBufferPreservesFrameAndDoesNotReleaseCapacity)
    {
        const std::array frame{ std::byte{ 42 }, std::byte{ 17 } };
        for (std::size_t i = 0; i < Transport::FRAME_CAPACITY; ++i)
            ASSERT_TRUE(run(endpoints[0]->send(frame)));
        auto blocked = launch(context, endpoints[0]->send(frame));
        drain();

        std::array tooSmall{ std::byte{ 0xAA } };
        EXPECT_EQ(run(endpoints[1]->receive(tooSmall)), std::unexpected(Error::BufferTooSmall));
        EXPECT_EQ(tooSmall[0], std::byte{ 0xAA });
        EXPECT_EQ(blocked.wait_for(0ms), std::future_status::timeout);

        std::array<std::byte, 2> received{};
        ASSERT_TRUE(run(endpoints[1]->receive(received)));
        EXPECT_EQ(received, frame);
        ASSERT_EQ(blocked.wait_for(0ms), std::future_status::ready);
        EXPECT_TRUE(blocked.get());
    }

    TEST_F(TransportTest, CancellingBlockedSendDoesNotQueueItsFrame)
    {
        fill();
        const std::array extra{ std::byte{ 99 } };
        std::stop_source stop;
        auto blocked = launch(context, endpoints[0]->send(extra, stop.get_token()));
        drain();
        EXPECT_EQ(blocked.wait_for(0ms), std::future_status::timeout);
        stop.request_stop();
        drain();
        ASSERT_EQ(blocked.wait_for(0ms), std::future_status::ready);
        EXPECT_EQ(blocked.get(), std::unexpected(Error::Cancelled));

        // Another blocked send can replace the cancelled waiter.
        auto replacement = launch(context, endpoints[0]->send(extra));
        drain();
        EXPECT_EQ(replacement.wait_for(0ms), std::future_status::timeout);
        std::array<std::byte, 1> received{};
        for (std::size_t i = 0; i < Transport::FRAME_CAPACITY; ++i) {
            ASSERT_TRUE(run(endpoints[1]->receive(received)));
            EXPECT_EQ(received[0], static_cast<std::byte>(i));
        }
        ASSERT_EQ(replacement.wait_for(0ms), std::future_status::ready);
        ASSERT_TRUE(replacement.get());
        ASSERT_TRUE(run(endpoints[1]->receive(received)));
        EXPECT_EQ(received, extra);

        std::stop_source finish;
        auto empty = launch(context, endpoints[1]->receive(received, finish.get_token()));
        drain();
        EXPECT_EQ(empty.wait_for(0ms), std::future_status::timeout);
        finish.request_stop();
        drain();
        EXPECT_EQ(empty.get(), std::unexpected(Error::Cancelled));
    }

    TEST_F(TransportTest, CancellationPreservesReceiveBufferAndQueuedFrames)
    {
        std::array buffer{ std::byte{ 0xAA } };
        std::stop_source stop;
        auto waiting = launch(context, endpoints[1]->receive(buffer, stop.get_token()));
        drain();
        stop.request_stop();
        drain();
        ASSERT_EQ(waiting.wait_for(0ms), std::future_status::ready);
        EXPECT_EQ(waiting.get(), std::unexpected(Error::Cancelled));
        EXPECT_EQ(buffer[0], std::byte{ 0xAA });

        const std::array frame{ std::byte{ 42 } };
        EXPECT_EQ(run(endpoints[0]->send(frame, stop.get_token())), std::unexpected(Error::Cancelled));
        ASSERT_TRUE(run(endpoints[0]->send(frame)));
        EXPECT_EQ(run(endpoints[1]->receive(buffer, stop.get_token())), std::unexpected(Error::Cancelled));
        EXPECT_EQ(buffer[0], std::byte{ 0xAA });
        ASSERT_TRUE(run(endpoints[1]->receive(buffer)));
        EXPECT_EQ(buffer, frame);
    }

    TEST_F(TransportTest, CancellingReceiveAfterNotificationLeavesTheFrameQueued)
    {
        std::stop_source stop;
        std::array buffer{ std::byte{ 0xAA } };
        auto receiving = launch(context, endpoints[1]->receive(buffer, stop.get_token()));
        drain();

        const std::array frame{ std::byte{ 42 } };
        auto sending = launch(context, endpoints[0]->send(frame));
        ASSERT_EQ(context.poll(1), 1U); // Enqueue and notify; don't resume the receiver yet.
        ASSERT_EQ(sending.wait_for(0ms), std::future_status::ready);
        ASSERT_TRUE(sending.get());
        stop.request_stop();
        drain();
        ASSERT_EQ(receiving.wait_for(0ms), std::future_status::ready);
        EXPECT_EQ(receiving.get(), std::unexpected(Error::Cancelled));
        EXPECT_EQ(buffer[0], std::byte{ 0xAA });
        ASSERT_TRUE(run(endpoints[1]->receive(buffer)));
        EXPECT_EQ(buffer, frame);
    }

    TEST_F(TransportTest, CancellingSendAfterSpaceNotificationDoesNotEnqueue)
    {
        fill();
        std::stop_source stop;
        const std::array frame{ std::byte{ 99 } };
        auto sending = launch(context, endpoints[0]->send(frame, stop.get_token()));
        drain();

        std::array<std::byte, 1> buffer{};
        auto receiving = launch(context, endpoints[1]->receive(buffer));
        ASSERT_EQ(context.poll(1), 1U); // Free space and notify; don't resume the sender yet.
        ASSERT_EQ(receiving.wait_for(0ms), std::future_status::ready);
        ASSERT_TRUE(receiving.get());
        EXPECT_EQ(buffer[0], std::byte{ 0 });
        stop.request_stop();
        drain();
        ASSERT_EQ(sending.wait_for(0ms), std::future_status::ready);
        EXPECT_EQ(sending.get(), std::unexpected(Error::Cancelled));

        for (std::size_t i = 1; i < Transport::FRAME_CAPACITY; ++i) {
            ASSERT_TRUE(run(endpoints[1]->receive(buffer)));
            EXPECT_EQ(buffer[0], static_cast<std::byte>(i));
        }
        auto empty = launch(context, endpoints[1]->receive(buffer));
        drain();
        EXPECT_EQ(empty.wait_for(0ms), std::future_status::timeout);
        endpoints[0]->close();
        drain();
        EXPECT_EQ(empty.get(), std::unexpected(Error::Disconnected));
    }

    TEST_F(TransportTest, ClosingWakesBlockedSendsAndDiscardsQueuedFrames)
    {
        fill(0);
        fill(1);
        const std::array frame{ std::byte{ 99 } };
        auto left = launch(context, endpoints[0]->send(frame));
        auto right = launch(context, endpoints[1]->send(frame));
        drain();
        EXPECT_EQ(left.wait_for(0ms), std::future_status::timeout);
        EXPECT_EQ(right.wait_for(0ms), std::future_status::timeout);
        endpoints[0]->close();
        endpoints[1]->close();
        drain();
        ASSERT_EQ(left.wait_for(0ms), std::future_status::ready);
        ASSERT_EQ(right.wait_for(0ms), std::future_status::ready);
        EXPECT_EQ(left.get(), std::unexpected(Error::Disconnected));
        EXPECT_EQ(right.get(), std::unexpected(Error::Disconnected));
        for (const auto& endpoint : endpoints) {
            std::array buffer{ std::byte{ 0xAA } };
            EXPECT_EQ(run(endpoint->receive(buffer)), std::unexpected(Error::Disconnected));
            EXPECT_EQ(buffer[0], std::byte{ 0xAA });
            EXPECT_EQ(run(endpoint->send(frame)), std::unexpected(Error::Disconnected));
        }
    }

    TEST_F(TransportTest, ClosingWakesBothEmptyReceivers)
    {
        std::array leftBuffer{ std::byte{ 0xAA } }, rightBuffer{ std::byte{ 0xBB } };
        auto left = launch(context, endpoints[0]->receive(leftBuffer));
        auto right = launch(context, endpoints[1]->receive(rightBuffer));
        drain();
        endpoints[1]->close();
        drain();
        ASSERT_EQ(left.wait_for(0ms), std::future_status::ready);
        ASSERT_EQ(right.wait_for(0ms), std::future_status::ready);
        EXPECT_EQ(left.get(), std::unexpected(Error::Disconnected));
        EXPECT_EQ(right.get(), std::unexpected(Error::Disconnected));
        EXPECT_EQ(leftBuffer[0], std::byte{ 0xAA });
        EXPECT_EQ(rightBuffer[0], std::byte{ 0xBB });
    }

    TEST_F(TransportTest, DestroyingPeerWakesSurvivingReceiver)
    {
        std::array buffer{ std::byte{ 0xAA } };
        auto waiting = launch(context, endpoints[0]->receive(buffer));
        drain();
        endpoints[1].reset();
        drain();
        ASSERT_EQ(waiting.wait_for(0ms), std::future_status::ready);
        EXPECT_EQ(waiting.get(), std::unexpected(Error::Disconnected));
        EXPECT_EQ(buffer[0], std::byte{ 0xAA });
    }

    pnm::coro::Task<void> exchange(Transport& endpoint, std::byte outgoing, std::byte incoming)
    {
        for (std::size_t i = 0; i < 2000; ++i) {
            const std::array frame{ outgoing, static_cast<std::byte>(i & 0xFFU) };
            if (!(co_await endpoint.send(frame)))
                throw std::runtime_error("Concurrent send failed");
            std::array<std::byte, 2> buffer{};
            const auto result = co_await endpoint.receive(buffer);
            if (!result || *result != buffer.size() || buffer[0] != incoming || buffer[1] != frame[1])
                throw std::runtime_error("Concurrent exchange lost or reordered a frame");
        }
    }

    TEST_F(TransportTest, ExchangesFramesAcrossTwoContextThreadsWithoutLostWakeups)
    {
        pnm::coro::Context peerContext;
        auto left = launch(context, exchange(*endpoints[0], std::byte{ 0xAA }, std::byte{ 0xBB }));
        auto right = launch(peerContext, exchange(*endpoints[1], std::byte{ 0xBB }, std::byte{ 0xAA }));
        std::jthread leftRunner([&] { context.run(); }), rightRunner([&] { peerContext.run(); });
        const auto leftStatus = left.wait_for(5s), rightStatus = right.wait_for(5s);
        endpoints[0]->close();
        // Keep both contexts running through disconnection cleanup, even on failure.
        left.wait();
        right.wait();
        context.stop();
        peerContext.stop();
        leftRunner.join();
        rightRunner.join();
        EXPECT_EQ(leftStatus, std::future_status::ready);
        EXPECT_EQ(rightStatus, std::future_status::ready);
        EXPECT_NO_THROW(left.get());
        EXPECT_NO_THROW(right.get());
    }

    enum class TopicPublisher
    {
        IO = 10,
        UI = 20
    };
    struct Topics
    {
        [[= TopicPublisher::IO]] static constexpr pnm::msg::TopicDefinition<std::string> state{
            "motor/state"
        };
        [[= TopicPublisher::UI]] static constexpr pnm::msg::TopicDefinition<std::string> status{ "ui/status" };
        static constexpr pnm::msg::TopicDefinition<std::string> local{ "local/status" };
    };

    TEST_F(TransportTest, RejectsUnknownPublisherBeforeRegisteringAnyTopics)
    {
        pnm::msg::Bus bus;
        p2p::Bridge bridge(bus, context, *endpoints[0]);
        EXPECT_THROW(bridge.registerTopics<Topics>(static_cast<TopicPublisher>(99)), std::invalid_argument);
        EXPECT_NO_THROW(bridge.registerTopics<Topics>(TopicPublisher::IO));
    }

    TEST_F(TransportTest, ConnectsTwoBridgesWithAnnotatedTopics)
    {
        pnm::msg::Bus ioBus, uiBus;
        p2p::Bridge io(ioBus, context, *endpoints[0]), ui(uiBus, context, *endpoints[1]);
        io.registerTopics<Topics>(TopicPublisher::IO);
        ui.registerTopics<Topics>(TopicPublisher::UI);
        std::string state, status;
        auto stateSubscription = Topics::state.on(uiBus).subscribe([&](const auto& value) { state = value; });
        auto statusSubscription =
          Topics::status.on(ioBus).subscribe([&](const auto& value) { status = value; });
        io.start();
        ui.start();
        Topics::local.on(ioBus).publish("stays local");
        Topics::local.on(uiBus).publish("also stays local");
        Topics::state.on(ioBus).publish("starting");
        Topics::state.on(ioBus).publish("initializing");
        Topics::state.on(ioBus).publish("idle");
        Topics::status.on(uiBus).publish("ready");
        drain();
        EXPECT_EQ(stateSubscription.poll(), 1U);
        EXPECT_EQ(statusSubscription.poll(), 1U);
        EXPECT_EQ(state, "idle");
        EXPECT_EQ(status, "ready");
        endpoints[0]->close();
        drain();
        io.stop();
        ui.stop();
        ASSERT_TRUE(io.failure());
        ASSERT_TRUE(ui.failure());
        EXPECT_EQ(std::get<Error>(*io.failure()), Error::Disconnected);
        EXPECT_EQ(std::get<Error>(*ui.failure()), Error::Disconnected);
    }
}
