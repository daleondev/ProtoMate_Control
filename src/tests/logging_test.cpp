#include "common/icc/bridge/Bridge.hpp"
#include "common/icc/catalog/topics.hpp"
#include "common/icc/transport/InProcessTransport.hpp"
#include "io/logging/Sink.hpp"
#include "ui/logging/Subscriber.hpp"
#include "runtime/thread.hpp"

#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <vector>

namespace
{
    using namespace std::chrono_literals;

    class LoggingTest : public testing::Test
    {
      protected:
        pnm::msg::Bus ioBus, uiBus;
        pnm::coro::Context ioContext, uiContext;
        decltype(icc::InProcessTransport::pair()) endpoints{ icc::InProcessTransport::pair() };
        icc::Bridge ioBridge{ ioBus, ioContext, *endpoints[0] };
        icc::Bridge uiBridge{ uiBus, uiContext, *endpoints[1] };
        std::shared_ptr<io::logging::Sink> sink{ std::make_shared<io::logging::Sink>(ioBus) };
        std::mutex mutex;
        std::vector<std::string> output;
        std::thread::id outputThread;
        ui::logging::Subscriber subscriber{ uiBus, [this](std::string_view line) {
            std::scoped_lock lock{ mutex };
            output.emplace_back(line);
            outputThread = std::this_thread::get_id();
        } };
        std::jthread worker;

        void SetUp() override
        {
            ioBridge.registerTopics<icc::Topics>(icc::TopicPublisher::IO);
            uiBridge.registerTopics<icc::Topics>(icc::TopicPublisher::UI);
        }
        void pump()
        {
            for (unsigned turn{}; turn < 10000; ++turn) {
                if (ioContext.poll() + uiContext.poll() == 0) { return; }
            }
            throw std::runtime_error("ICC did not become idle");
        }
        void connect()
        {
            ioBridge.start();
            uiBridge.start();
            pump();
            ASSERT_EQ(ioBridge.connectionStatus().state, icc::ConnectionState::Connected);
            ASSERT_EQ(uiBridge.connectionStatus().state, icc::ConnectionState::Connected);
        }
        void startWorker()
        {
            worker = runtime::thread::create_jthread(
              { .name = "log test", .priority = 20, .stack_size = 16384 },
              [this](std::stop_token stop) { subscriber.run(stop); });
        }
        bool waitForOutput(std::size_t count)
        {
            const auto deadline{ std::chrono::steady_clock::now() + 2s };
            while (std::chrono::steady_clock::now() < deadline) {
                {
                    std::scoped_lock lock{ mutex };
                    if (output.size() == count) { return true; }
                }
                std::this_thread::sleep_for(1ms);
            }
            return false;
        }
        void write(std::string_view line)
        {
            const auto result{ sink->write(std::as_bytes(std::span{ line })) };
            ASSERT_TRUE(result);
            EXPECT_EQ(*result, line.size());
        }
        void TearDown() override
        {
            worker.request_stop();
            if (worker.joinable()) { worker.join(); }
            EXPECT_TRUE(sink->close());
            endpoints[0]->close();
            pump();
            ioBridge.stop();
            uiBridge.stop();
        }
    };

    TEST_F(LoggingTest, StartupRecordsRetainFormattingAndRunOnOutputWorker)
    {
        auto observe{ ioBus.topic<^^icc::Topics::io_logs>().subscribe([](const auto&) {}) };
        pnm::log::debug(pnm::log::immediate, sink, "initializing motor {}", 2);
        write("[01:02:03] Warn: driver fault\n");
        connect();
        // Receiving frames must not write to the console from the ICC context.
        EXPECT_TRUE(output.empty());
        startWorker();
        ASSERT_TRUE(waitForOutput(2));
        {
            std::scoped_lock lock{ mutex };
            EXPECT_TRUE(output[0].starts_with("[IO] ["));
            EXPECT_NE(output[0].find("Debug: initializing motor 2\n"), std::string::npos);
            EXPECT_EQ(output[0].find('\x1b'), std::string::npos);
            EXPECT_EQ(output[1], "[IO] [01:02:03] Warn: driver fault\n");
            EXPECT_NE(outputThread, std::this_thread::get_id());
        }
        // UI writes directly; it must not feed received records back into IO.
        pump();
        EXPECT_EQ(observe.poll(), 2U);
    }

    TEST_F(LoggingTest, QueuesStayBoundedWithoutPeerOrConsoleWorker)
    {
        for (unsigned i{}; i < 40; ++i) { write(std::to_string(i)); }
        auto stats{ ioBridge.topicStatistics("io/logs") };
        ASSERT_TRUE(stats);
        EXPECT_EQ(stats->queued, 16U);
        EXPECT_EQ(stats->dropped, 24U);
        connect();
        EXPECT_EQ(subscriber.statistics().queued, 16U);
        for (unsigned i{ 40 }; i < 80; ++i) {
            write(std::to_string(i));
            pump();
        }
        EXPECT_EQ(subscriber.statistics().queued, 16U);
        EXPECT_EQ(subscriber.statistics().dropped, 40U);
        startWorker();
        ASSERT_TRUE(waitForOutput(16));
        std::scoped_lock lock{ mutex };
        for (std::size_t i{}; i < output.size(); ++i) {
            EXPECT_EQ(output[i], "[IO] " + std::to_string(64 + i) + '\n');
        }
    }

    TEST_F(LoggingTest, LongRecordsAreTruncatedOnceAndFitBackgroundFrames)
    {
        write(std::string(4096, 'x'));
        write("following record\n");
        EXPECT_EQ(sink->truncatedRecords(), 1U);
        connect();
        EXPECT_EQ(ioBridge.topicStatistics("io/logs")->dropped, 0U);
        startWorker();
        ASSERT_TRUE(waitForOutput(2));
        std::scoped_lock lock{ mutex };
        EXPECT_EQ(output[0].size(), icc::MAX_LOG_BYTES + 5);
        EXPECT_TRUE(output[0].ends_with("... [truncated]\n"));
        EXPECT_EQ(output[1], "[IO] following record\n");
    }

    TEST_F(LoggingTest, ConcurrentPublishingDoesNotLoseWakeups)
    {
        connect();
        startWorker();
        // Allow the initial readiness notification to be consumed first.
        std::this_thread::sleep_for(10ms);
        for (unsigned i{}; i < 50; ++i) {
            write("wake " + std::to_string(i));
            pump();
            ASSERT_TRUE(waitForOutput(i + 1));
        }
        EXPECT_EQ(subscriber.statistics().dropped, 0U);
        worker.request_stop();
        worker.join(); // Cancellation wakes an idle worker, no periodic timeout.
    }

    TEST(LoggingLifetime, ClosedSinkCannotReopenOrPublishAfterBusDestruction)
    {
        std::shared_ptr<io::logging::Sink> sink;
        {
            pnm::msg::Bus bus;
            sink = std::make_shared<io::logging::Sink>(bus);
            EXPECT_TRUE(sink->isOpen());
            EXPECT_TRUE(sink->close());
        }
        EXPECT_FALSE(sink->isOpen());
        EXPECT_FALSE(sink->open());
        EXPECT_FALSE(sink->write(std::as_bytes(std::span{ "late entry" })));
        // The backend automatically tries open() for closed sinks. Shutdown
        // must not reattach it or call any destroyed application object.
        pnm::log::warn(pnm::log::immediate, sink, "late queued record");
        EXPECT_FALSE(sink->isOpen());
    }
}
