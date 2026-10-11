#include "p2p/Bridge.hpp"
#include "p2p/InProcessTransport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using namespace std::chrono_literals;
    using Error = pnm::msg::ServiceError;
    using Reply = pnm::msg::Reply<std::string>;
    using Call = pnm::msg::PendingCall<std::string>;

    enum class Provider { IO = 10, UI = 20 };
    struct Services
    {
        [[= Provider::IO]] static constexpr pnm::msg::ServiceDefinition<std::string, std::string> echo{"io/echo"};
        [[= Provider::UI]] static constexpr pnm::msg::ServiceDefinition<std::string, std::string> status{"ui/status"};
        static constexpr pnm::msg::ServiceDefinition<std::string, std::string> local{"local"};
    };

    class ServiceTest : public ::testing::Test
    {
      protected:
        pnm::coro::Context ioContext, uiContext;
        std::array<std::shared_ptr<p2p::InProcessTransport>, 2> endpoints{p2p::InProcessTransport::pair()};
        pnm::msg::Bus ioBus, uiBus;
        p2p::Bridge io{ioBus, ioContext, *endpoints[0]}, ui{uiBus, uiContext, *endpoints[1]};

        void SetUp() override
        {
            io.registerServices<Services>(Provider::IO);
            ui.registerServices<Services>(Provider::UI);
            io.start();
            ui.start();
        }

        void drain()
        {
            for (unsigned i = 0; i < 1000; ++i) {
                const auto work = ioContext.poll() + uiContext.poll();
                if (work == 0)
                    return;
            }
            FAIL() << "Bridge did not become idle";
        }

        void TearDown() override
        {
            endpoints[0]->close();
            drain();
            io.stop();
            ui.stop();
        }
    };

    TEST_F(ServiceTest, CatalogRoutesRequestsToBothOwnersAndKeepsUnannotatedServicesLocal)
    {
        auto ioServer = Services::echo.on(ioBus).serve([](const auto& value) { return "io:" + value; });
        auto uiServer = Services::status.on(uiBus).serve([](const auto& value) { return "ui:" + value; });
        auto localServer = Services::local.on(ioBus).serve([](const auto& value) { return value; });
        auto left = Services::echo.on(uiBus).request("one", 1s);
        auto right = Services::status.on(ioBus).request("two", 1s);
        auto local = Services::local.on(uiBus).request("local", 1s);
        EXPECT_EQ(local.get(), std::unexpected(Error::Unavailable));
        drain();
        EXPECT_FALSE(left.ready());
        EXPECT_FALSE(right.ready());
        EXPECT_EQ(ioServer.poll(), 1U);
        EXPECT_EQ(uiServer.poll(), 1U);
        drain();
        ASSERT_TRUE(left.ready());
        ASSERT_TRUE(right.ready());
        EXPECT_EQ(left.get(), "io:one");
        EXPECT_EQ(right.get(), "ui:two");
        EXPECT_FALSE(io.failure());
        EXPECT_FALSE(ui.failure());
    }

    TEST_F(ServiceTest, DeferredRepliesAreCorrelatedWhenCompletedOutOfOrder)
    {
        std::vector<Reply> replies;
        auto server = Services::echo.on(ioBus).serveDeferred([&](const auto&, Reply reply) {
            replies.push_back(std::move(reply));
        });
        auto first = Services::echo.on(uiBus).request("one", 1s);
        auto second = Services::echo.on(uiBus).request("two", 1s);
        drain();
        ASSERT_EQ(server.poll(), 2U);
        ASSERT_EQ(replies.size(), 2U);
        replies[1].respond("second");
        drain();
        ASSERT_TRUE(second.ready());
        EXPECT_EQ(second.get(), "second");
        EXPECT_FALSE(first.ready());
        replies[0].respond("first");
        drain();
        ASSERT_TRUE(first.ready());
        EXPECT_EQ(first.get(), "first");
    }

    TEST_F(ServiceTest, PreservesEmptyEmbeddedNullAndMaximumPayloads)
    {
        auto server = Services::echo.on(ioBus).serve([](const auto& value) { return value; });
        for (const auto& value : {std::string{}, std::string{"a\0b", 3}, std::string(2048 - 16 - 7, 'x')}) {
            auto call = Services::echo.on(uiBus).request(value, 1s);
            drain();
            ASSERT_EQ(server.poll(), 1U);
            drain();
            ASSERT_TRUE(call.ready());
            EXPECT_EQ(call.get(), value);
        }
    }

    TEST_F(ServiceTest, ReportsMissingProviderAndHandlerErrorsWithoutBreakingConnection)
    {
        auto missing = Services::echo.on(uiBus).request("missing", 1s);
        drain();
        ASSERT_TRUE(missing.ready());
        EXPECT_EQ(missing.get(), std::unexpected(Error::Unavailable));
        auto server = Services::echo.on(ioBus).serveDeferred([](const std::string& value, Reply reply) {
            if (value == "throw") throw std::runtime_error("handler failure");
            reply.fail(Error::Busy);
        });
        for (const auto& value : {std::string{"throw"}, std::string{"busy"}}) {
            auto call = Services::echo.on(uiBus).request(value, 1s);
            drain();
            EXPECT_EQ(server.poll(), 1U);
            drain();
            ASSERT_TRUE(call.ready());
            EXPECT_EQ(call.get(), std::unexpected(value == "throw" ? Error::HandlerFailed : Error::Busy));
        }
        EXPECT_FALSE(ui.failure());
        EXPECT_FALSE(io.failure());
    }

    TEST_F(ServiceTest, RejectsOversizedRequestsAndResponsesWithoutFailingOtherCalls)
    {
        auto server = Services::echo.on(ioBus).serve([](const auto& value) {
            return value == "large" ? std::string(2049, 'x') : value;
        });
        auto request = Services::echo.on(uiBus).request(std::string(2049, 'x'), 1s);
        drain();
        ASSERT_TRUE(request.ready());
        EXPECT_EQ(request.get(), std::unexpected(Error::TransportError));
        EXPECT_EQ(server.poll(), 0U);
        auto response = Services::echo.on(uiBus).request("large", 1s);
        drain();
        ASSERT_EQ(server.poll(), 1U);
        drain();
        ASSERT_TRUE(response.ready());
        EXPECT_EQ(response.get(), std::unexpected(Error::TransportError));
        EXPECT_FALSE(io.failure());
        EXPECT_FALSE(ui.failure());
    }

    TEST_F(ServiceTest, BoundsPendingCallsAndReusesCapacityAfterCompletion)
    {
        auto server = Services::echo.on(ioBus).serve([](const auto& value) { return value; });
        std::vector<Call> calls;
        for (unsigned i = 0; i < 8; ++i)
            calls.push_back(Services::echo.on(uiBus).request(std::to_string(i), 1s));
        auto excess = Services::echo.on(uiBus).request("too many", 1s);
        ASSERT_TRUE(excess.ready());
        EXPECT_EQ(excess.get(), std::unexpected(Error::Busy));
        drain();
        ASSERT_EQ(server.poll(), 8U);
        drain();
        for (unsigned i = 0; i < calls.size(); ++i) {
            ASSERT_TRUE(calls[i].ready());
            EXPECT_EQ(calls[i].get(), std::to_string(i));
        }
        auto next = Services::echo.on(uiBus).request("next", 1s);
        drain();
        ASSERT_EQ(server.poll(), 1U);
        drain();
        ASSERT_TRUE(next.ready());
        EXPECT_EQ(next.get(), "next");
    }

    TEST_F(ServiceTest, CancellationBeforeDispatchPreventsHandlerExecution)
    {
        auto server = Services::echo.on(ioBus).serve([](const auto& value) { return value; });
        auto call = Services::echo.on(uiBus).request("cancel", 1s);
        drain(); // Request is now queued on the IO bus, but handler has not run.
        EXPECT_TRUE(call.cancel());
        drain();
        EXPECT_EQ(server.poll(), 0U);
        EXPECT_EQ(call.get(), std::unexpected(Error::Cancelled));
    }

    TEST_F(ServiceTest, CancellingDeferredRequestInvalidatesRemoteReply)
    {
        std::optional<Reply> reply;
        auto server = Services::echo.on(ioBus).serveDeferred([&](const auto&, Reply value) {
            reply.emplace(std::move(value));
        });
        std::stop_source stop;
        auto call = Services::echo.on(uiBus).request("cancel", 1s, stop.get_token());
        drain();
        ASSERT_EQ(server.poll(), 1U);
        stop.request_stop();
        drain();
        EXPECT_FALSE(reply->pending());
        EXPECT_FALSE(reply->respond("late"));
        EXPECT_EQ(call.get(), std::unexpected(Error::Cancelled));
        EXPECT_FALSE(io.failure());
    }

    TEST_F(ServiceTest, TimerExpiresDeferredCallWithoutApplicationPolling)
    {
        std::optional<Reply> reply;
        auto server = Services::echo.on(ioBus).serveDeferred([&](const auto&, Reply value) {
            reply.emplace(std::move(value));
        });
        auto call = Services::echo.on(uiBus).request("timeout", 20ms);
        drain();
        ASSERT_EQ(server.poll(), 1U);
        std::this_thread::sleep_for(30ms);
        drain(); // Timers wake the bridge even though nobody touched the handles.
        EXPECT_FALSE(reply->respond("late"));
        ASSERT_TRUE(call.ready());
        EXPECT_EQ(call.get(), std::unexpected(Error::Timeout));
        EXPECT_FALSE(ui.failure());
    }

    TEST_F(ServiceTest, TimeoutBeforeForwardingNeverInvokesProvider)
    {
        auto server = Services::echo.on(ioBus).serve([](const auto& value) { return value; });
        auto call = Services::echo.on(uiBus).request("expired", 1ms);
        std::this_thread::sleep_for(5ms);
        drain();
        EXPECT_EQ(server.poll(), 0U);
        EXPECT_EQ(call.get(), std::unexpected(Error::Timeout));
    }

    TEST_F(ServiceTest, LateResponseAfterCancellationCannotCompleteAnotherCall)
    {
        auto server = Services::echo.on(ioBus).serve([](const auto& value) { return value; });
        auto old = Services::echo.on(uiBus).request("old", 1s);
        drain();
        ASSERT_EQ(server.poll(), 1U);
        ioContext.poll(); // Queue response without letting UI receive it yet.
        old.cancel();
        auto next = Services::echo.on(uiBus).request("next", 1s);
        drain();
        EXPECT_EQ(old.get(), std::unexpected(Error::Cancelled));
        EXPECT_FALSE(next.ready());
        ASSERT_EQ(server.poll(), 1U);
        drain();
        ASSERT_TRUE(next.ready());
        EXPECT_EQ(next.get(), "next");
    }

    TEST_F(ServiceTest, PeerDisconnectFailsActiveAndQueuedProxyCalls)
    {
        auto server = Services::echo.on(ioBus).serve([](const auto& value) { return value; });
        auto active = Services::echo.on(uiBus).request("active", 1s);
        drain();
        auto queued = Services::echo.on(uiBus).request("queued", 1s);
        endpoints[0]->close();
        drain();
        ASSERT_TRUE(active.ready());
        ASSERT_TRUE(queued.ready());
        EXPECT_EQ(active.get(), std::unexpected(Error::TransportError));
        EXPECT_EQ(queued.get(), std::unexpected(Error::TransportError));
        EXPECT_EQ(server.poll(), 0U);
    }

    TEST_F(ServiceTest, ClosingFullTransportDrainsBlockedSenderAndCancelledCalls)
    {
        std::vector<Call> calls;
        for (unsigned i = 0; i < 8; ++i)
            calls.push_back(Services::echo.on(uiBus).request("queued", 1s));
        uiContext.poll(); // Fill the transport with requests while IO is paused.
        for (auto& call : calls) call.cancel();
        uiContext.poll(); // Sender is now waiting for room to send cancellation.
        endpoints[1]->close();
        drain();
        for (auto& call : calls)
            EXPECT_EQ(call.get(), std::unexpected(Error::Cancelled));
        ASSERT_TRUE(ui.failure());
    }


    TEST(ServiceLifecycle, StopDrainsLocalCallsAndCancelsIncomingCallsBeforeReturning)
    {
        pnm::msg::Bus ioBus, uiBus;
        pnm::coro::Context context;
        auto endpoints = p2p::InProcessTransport::pair();
        std::jthread runner([&](std::stop_token stop) {
            std::stop_callback cancellation{stop, [&] { context.stop(); }};
            context.run();
        });
        p2p::Bridge io(ioBus, context, *endpoints[0]), ui(uiBus, context, *endpoints[1]);
        io.registerServices<Services>(Provider::IO);
        ui.registerServices<Services>(Provider::UI);
        std::optional<Reply> reply;
        auto server = Services::echo.on(ioBus).serveDeferred([&](const auto&, Reply value) {
            reply.emplace(std::move(value));
        });
        auto peerServer = Services::status.on(uiBus).serve([](const auto& value) { return value; });
        io.start();
        ui.start();
        auto incoming = Services::echo.on(uiBus).request("incoming", 1s);
        ASSERT_EQ(server.poll(1s), 1U);
        auto outgoing = Services::status.on(ioBus).request("outgoing", 1s);
        io.stop();
        EXPECT_FALSE(reply->pending());
        ASSERT_TRUE(outgoing.ready());
        EXPECT_EQ(outgoing.get(), std::unexpected(Error::Cancelled));
        ui.stop();
        EXPECT_FALSE(io.failure());
        EXPECT_FALSE(ui.failure());
        EXPECT_EQ(incoming.get(), std::unexpected(Error::Cancelled));
        EXPECT_EQ(peerServer.poll(), 0U);
        auto afterStop = Services::status.on(ioBus).request("unavailable", 1s);
        EXPECT_EQ(afterStop.get(), std::unexpected(Error::Unavailable));
    }

    TEST(ServiceLifecycle, ForwardsServicesAndTopicsOnSeparateContextThreads)
    {
        pnm::msg::Bus ioBus, uiBus;
        pnm::coro::Context ioContext, uiContext;
        auto endpoints = p2p::InProcessTransport::pair();
        std::jthread ioRunner([&](std::stop_token stop) {
            std::stop_callback cancellation{stop, [&] { ioContext.stop(); }};
            ioContext.run();
        });
        std::jthread uiRunner([&](std::stop_token stop) {
            std::stop_callback cancellation{stop, [&] { uiContext.stop(); }};
            uiContext.run();
        });
        p2p::Bridge io(ioBus, ioContext, *endpoints[0]), ui(uiBus, uiContext, *endpoints[1]);
        io.registerServices<Services>(Provider::IO);
        ui.registerServices<Services>(Provider::UI);
        io.registerOutgoingTopic<std::string>("state");
        ui.registerIncomingTopic<std::string>("state");
        std::string state;
        auto subscription = uiBus.topic<std::string>("state").subscribe([&](const auto& value) { state = value; });
        auto server = Services::echo.on(ioBus).serve([](const auto& value) { return value; });
        io.start();
        ui.start();
        for (unsigned i = 0; i < 100; ++i) {
            auto value = std::to_string(i);
            ioBus.topic<std::string>("state").publish(value);
            auto call = Services::echo.on(uiBus).request(value, 1s);
            ASSERT_EQ(server.poll(1s), 1U);
            EXPECT_EQ(call.get(), value);
            ASSERT_EQ(subscription.poll(1s), 1U);
            EXPECT_EQ(state, value);
        }
        io.stop();
        ui.stop();
        EXPECT_FALSE(io.failure());
        EXPECT_FALSE(ui.failure());
    }

    TEST(ServiceRegistration, ValidatesOwnershipAndDuplicateNamesBeforeStarting)
    {
        pnm::coro::Context context;
        pnm::msg::Bus bus;
        auto endpoints = p2p::InProcessTransport::pair();
        p2p::Bridge bridge(bus, context, *endpoints[0]);
        EXPECT_THROW(bridge.registerServices<Services>(static_cast<Provider>(99)), std::invalid_argument);
        EXPECT_NO_THROW(bridge.registerServices<Services>(Provider::IO));
        EXPECT_THROW((bridge.registerIncomingService<std::string, std::string>("io/echo")), std::invalid_argument);
        EXPECT_THROW((bridge.registerOutgoingService<std::string, std::string>("")), std::invalid_argument);
    }
}
