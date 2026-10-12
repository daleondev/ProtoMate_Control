#include "common/icc/bridge/Bridge.hpp"
#include "common/icc/transport/InProcessTransport.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using namespace std::chrono_literals;
    using icc::Lane;
    enum class Core { IO, UI };
    struct Topics
    {
        [[= Core::IO, = Lane::Background, = pnm::msg::SubscriptionOptions{ .capacity = 4 }]]
        pnm::msg::Topic<std::string> logs;
        [[= Core::IO, = Lane::Background,
          = pnm::msg::SubscriptionOptions{ 4, pnm::msg::OverflowPolicy::DropNewest }]]
        pnm::msg::Topic<std::string> oldest;
        [[= Core::IO]] pnm::msg::Topic<unsigned> state;
    };
    struct Services
    {
        [[= Core::IO]] pnm::msg::Service<unsigned, unsigned> normal;
        [[= Core::IO, = Lane::Urgent]] pnm::msg::Service<unsigned, unsigned> stop;
    };
    struct Actions
    {
        [[= Core::IO]] pnm::msg::Action<unsigned, unsigned, unsigned> move;
    };

    class GatedBackground final : public icc::ITransport
    {
      public:
        explicit GatedBackground(ITransport& inner) : m_inner(inner) {}
        void block() { m_gate.emplace(); }
        void release()
        {
            if (auto gate = std::exchange(m_gate, std::nullopt)) { gate->close(); }
        }
        bool waiting{};
        pnm::coro::Task<pnm::EmptyResult<Error>> send(std::span<const std::byte> frame, Lane lane,
                                                    std::stop_token stop) override
        {
            if (lane == Lane::Background && m_gate) {
                auto gate = *m_gate;
                waiting = true;
                co_await gate.next(stop);
                waiting = false;
            }
            co_return co_await m_inner.send(frame, lane, stop);
        }
        pnm::coro::Task<pnm::Result<std::size_t, Error>> receive(std::span<std::byte> buffer, Lane lane,
                                                              std::stop_token stop) override
        {
            co_return co_await m_inner.receive(buffer, lane, stop);
        }
      private:
        ITransport& m_inner;
        std::optional<pnm::coro::Channel<bool>> m_gate;
    };

    class BackgroundTest : public testing::Test
    {
      protected:
        pnm::msg::Bus ioBus, uiBus;
        pnm::coro::Context ioContext, uiContext;
        std::array<std::shared_ptr<icc::InProcessTransport>, 2> endpoints{ icc::InProcessTransport::pair() };
        GatedBackground transport{ *endpoints[0] };
        icc::Bridge io{ ioBus, ioContext, transport }, ui{ uiBus, uiContext, *endpoints[1] };

        void SetUp() override
        {
            io.registerTopics<Topics>(Core::IO);
            ui.registerTopics<Topics>(Core::UI);
            io.registerServices<Services>(Core::IO);
            ui.registerServices<Services>(Core::UI);
            io.registerActions<Actions>(Core::IO);
            ui.registerActions<Actions>(Core::UI);
        }
        void pump()
        {
            for (unsigned turn{}; turn < 10000; ++turn) {
                if (ioContext.poll() + uiContext.poll() == 0) { return; }
            }
            throw std::runtime_error("Bridge did not become idle");
        }
        void connect()
        {
            io.start();
            ui.start();
            pump();
            ASSERT_EQ(io.connectionStatus().state, icc::ConnectionState::Connected);
            ASSERT_EQ(ui.connectionStatus().state, icc::ConnectionState::Connected);
        }
        void TearDown() override
        {
            transport.release();
            endpoints[0]->close();
            pump();
            io.stop();
            ui.stop();
        }
    };

    TEST_F(BackgroundTest, StartupBurstsRetainBoundedFifoWithVisibleLoss)
    {
        std::vector<std::string> recent, oldest;
        auto a = uiBus.topic<^^Topics::logs>().subscribe([&](const auto& value) { recent.push_back(value); }, { .capacity = 16 });
        auto b = uiBus.topic<^^Topics::oldest>().subscribe([&](const auto& value) { oldest.push_back(value); }, { .capacity = 16 });
        for (unsigned i{}; i < 10; ++i) {
            ioBus.topic<^^Topics::logs>().publish(std::to_string(i));
            ioBus.topic<^^Topics::oldest>().publish(std::to_string(i));
        }
        ASSERT_TRUE(io.topicStatistics("logs"));
        EXPECT_EQ(io.topicStatistics("logs")->queued, 4U);
        EXPECT_EQ(io.topicStatistics("logs")->dropped, 6U);
        connect();
        EXPECT_EQ(a.poll(), 4U);
        EXPECT_EQ(b.poll(), 4U);
        EXPECT_EQ(recent, (std::vector<std::string>{ "6", "7", "8", "9" }));
        EXPECT_EQ(oldest, (std::vector<std::string>{ "0", "1", "2", "3" }));
        EXPECT_EQ(io.topicStatistics("logs")->queued, 0U);
        EXPECT_EQ(io.topicStatistics("logs")->dropped, 6U);
        EXPECT_FALSE(ui.topicStatistics("logs"));
        EXPECT_FALSE(io.topicStatistics("missing"));
    }

    TEST_F(BackgroundTest, OversizedLogsAreCountedAndFollowingRecordsStillArrive)
    {
        std::vector<std::string> received;
        auto subscription = uiBus.topic<^^Topics::logs>().subscribe([&](const auto& value) { received.push_back(value); });
        // Exactly 512 bytes including the 16-byte header and four-byte topic name.
        const std::string maximum(icc::MAX_BACKGROUND_FRAME_BYTES - icc::util::HEADER_BYTES - 4, 'x');
        ioBus.topic<^^Topics::logs>().publish(maximum);
        ioBus.topic<^^Topics::logs>().publish(maximum + 'x');
        ioBus.topic<^^Topics::logs>().publish(std::string(3000, 'x'));
        ioBus.topic<^^Topics::logs>().publish("still alive");
        connect();
        EXPECT_EQ(subscription.poll(), 2U);
        EXPECT_EQ(received, (std::vector<std::string>{ maximum, "still alive" }));
        EXPECT_EQ(io.topicStatistics("logs")->dropped, 2U);
        EXPECT_FALSE(io.failure());
        EXPECT_FALSE(ui.failure());
    }

    TEST_F(BackgroundTest, BlockedLoggingDoesNotDelayServicesActionCancellationOrHeartbeat)
    {
        auto normal = ioBus.service<^^Services::normal>().serve([](unsigned value) { return value + 1; });
        auto stop = ioBus.service<^^Services::stop>().serve([](unsigned value) { return value + 2; });
        std::optional<pnm::msg::ActionExecution<unsigned, unsigned>> execution;
        auto action = ioBus.action<^^Actions::move>().serveDeferred([&](unsigned, auto value) {
            value.accept();
            execution.emplace(std::move(value));
        });
        std::vector<std::string> received;
        auto logs = uiBus.topic<^^Topics::logs>().subscribe([&](const auto& value) { received.push_back(value); });
        connect();
        transport.block();
        ioBus.topic<^^Topics::logs>().publish("in flight");
        pump();
        ASSERT_TRUE(transport.waiting);
        for (unsigned i{}; i < 100; ++i) { ioBus.topic<^^Topics::logs>().publish(std::to_string(i)); }
        EXPECT_EQ(io.topicStatistics("logs")->queued, 4U);
        EXPECT_EQ(io.topicStatistics("logs")->dropped, 96U);
        auto ordinary = uiBus.service<^^Services::normal>().request(10U, 1s);
        auto urgent = uiBus.service<^^Services::stop>().request(10U, 1s);
        std::optional<pnm::msg::ActionResult<unsigned>> result;
        auto goal = uiBus.action<^^Actions::move>().sendGoal(10U, { .accept_timeout = 1s },
          { .on_result = [&](auto value) { result.emplace(std::move(value)); } });
        pump();
        EXPECT_EQ(normal.poll(), 1U);
        EXPECT_EQ(stop.poll(), 1U);
        EXPECT_EQ(action.poll(), 1U);
        pump();
        ASSERT_TRUE(ordinary.ready());
        ASSERT_TRUE(urgent.ready());
        EXPECT_EQ(ordinary.get(), 11U);
        EXPECT_EQ(urgent.get(), 12U);
        ASSERT_TRUE(execution);
        goal.requestCancel();
        pump();
        EXPECT_TRUE(execution->cancelRequested());
        EXPECT_FALSE(goal.ready());
        execution->cancelled(42);
        pump();
        goal.poll();
        ASSERT_TRUE(result && *result);
        EXPECT_EQ((**result).status, pnm::msg::ActionStatus::Cancelled);
        const auto until = std::chrono::steady_clock::now() + 600ms;
        while (std::chrono::steady_clock::now() < until) {
            pump();
            std::this_thread::sleep_for(1ms);
        }
        EXPECT_EQ(io.connectionStatus().state, icc::ConnectionState::Connected);
        EXPECT_EQ(ui.connectionStatus().state, icc::ConnectionState::Connected);
        EXPECT_TRUE(transport.waiting);
        transport.release();
        pump();
        EXPECT_EQ(logs.poll(), 5U);
        EXPECT_EQ(received, (std::vector<std::string>{ "in flight", "96", "97", "98", "99" }));
        EXPECT_FALSE(io.failure());
        EXPECT_FALSE(ui.failure());
    }

    TEST_F(BackgroundTest, ShutdownCancelsBlockedBackgroundWithoutWaitingForDelivery)
    {
        connect();
        transport.block();
        ioBus.topic<^^Topics::logs>().publish("blocked");
        pump();
        ASSERT_TRUE(transport.waiting);
        std::atomic_bool stopped{};
        std::jthread stopper([&] { io.stop(1s); stopped.store(true); });
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!stopped.load() && std::chrono::steady_clock::now() < deadline) {
            pump();
            std::this_thread::sleep_for(1ms);
        }
        EXPECT_TRUE(stopped.load());
        if (!stopped.load()) { endpoints[0]->close(); transport.release(); pump(); }
        stopper.join();
        pump();
        ui.stop();
        EXPECT_FALSE(io.failure());
        EXPECT_FALSE(ui.failure());
        EXPECT_FALSE(transport.waiting);
        EXPECT_FALSE(io.topicStatistics("logs"));
    }

    TEST_F(BackgroundTest, IncomingBackgroundTopicMustMatchItsRegisteredLane)
    {
        connect();
        auto frame = icc::util::encode_frame(icc::util::FrameKind::Topic, "state", 0, 42U);
        auto send = [](icc::ITransport& transport, std::span<const std::byte> bytes) -> pnm::coro::Task<void> {
            EXPECT_TRUE(co_await transport.send(bytes, Lane::Background));
        };
        pnm::coro::co_spawn(ioContext, send(*endpoints[0], std::span(frame.bytes).first(frame.size)));
        pump();
        EXPECT_TRUE(ui.failure());
        EXPECT_EQ(ui.connectionStatus().reason, icc::ConnectionReason::ProtocolError);
    }
}
