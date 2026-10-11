#include "p2p/bridge/Bridge.hpp"
#include "../examples/ContextThread.hpp"
#include "p2p/transport/InProcessTransport.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace
{
    using namespace std::chrono_literals;
    using State = p2p::ConnectionState;
    using Reason = p2p::ConnectionReason;
    using Kind = p2p::util::FrameKind;
    using p2p::Lane;
    using Execution = pnm::msg::ActionExecution<unsigned, unsigned>;
    enum class Provider { IO, UI };
    struct Services
    {
        [[= Provider::IO]] pnm::msg::Service<unsigned, unsigned> normal;
        [[= Provider::IO, = Lane::Urgent]] pnm::msg::Service<unsigned, unsigned> urgent;
    };
    struct Actions { [[= Provider::IO]] pnm::msg::Action<unsigned, unsigned, unsigned> move; };

    class FaultTransport final : public p2p::ITransport
    {
      public:
        explicit FaultTransport(ITransport& inner) : m_inner(inner) {}
        std::optional<Lane> blocked;
        bool dropAcknowledgements{}, waiting{};
        unsigned heartbeats{}, acknowledgements{};
        pnm::coro::Task<pnm::EmptyResult<Error>> send(
          std::span<const std::byte> bytes, Lane lane = Lane::Normal, std::stop_token stop = {}) override
        {
            if (blocked == lane) {
                waiting = true;
                co_await m_gate.next(stop);
                waiting = false;
            }
            if (stop.stop_requested()) co_return std::unexpected(Error::Cancelled);
            const auto kind = static_cast<Kind>(bytes[1]);
            if (kind == Kind::Heartbeat) ++heartbeats;
            if (kind == Kind::HeartbeatAck) {
                ++acknowledgements;
                if (dropAcknowledgements) co_return pnm::EmptyResult<Error>{};
            }
            co_return co_await m_inner.send(bytes, lane, stop);
        }
        pnm::coro::Task<pnm::Result<std::size_t, Error>> receive(
          std::span<std::byte> bytes, Lane lane = Lane::Normal, std::stop_token stop = {}) override
        { co_return co_await m_inner.receive(bytes, lane, stop); }

      private:
        ITransport& m_inner;
        pnm::coro::Channel<bool> m_gate;
    };

    class ConnectionTest : public ::testing::Test
    {
      protected:
        pnm::msg::Bus ioBus, uiBus;
        pnm::coro::Context ioContext, uiContext;
        std::array<std::shared_ptr<p2p::InProcessTransport>, 2> endpoints{p2p::InProcessTransport::pair()};
        FaultTransport ioTransport{*endpoints[0]}, uiTransport{*endpoints[1]};
        std::vector<p2p::ConnectionStatus> ioEvents, uiEvents;
        p2p::Bridge io{ioBus, ioContext, ioTransport, {20ms, 120ms}};
        p2p::Bridge ui{uiBus, uiContext, uiTransport, {20ms, 120ms}};

        void SetUp() override
        {
            io.registerServices<Services>(Provider::IO); ui.registerServices<Services>(Provider::UI);
            io.registerActions<Actions>(Provider::IO); ui.registerActions<Actions>(Provider::UI);
            io.setConnectionCallback([this](auto status) {
                // Reentrant reads prove callbacks run outside internal locks.
                EXPECT_EQ(io.connectionStatus(), status);
                if (status.state == State::Failed) EXPECT_TRUE(io.failure());
                ioEvents.push_back(status);
            });
            ui.setConnectionCallback([this](auto status) { uiEvents.push_back(status); });
        }
        static void drain(pnm::coro::Context& context) { while (context.poll()) {} }
        void pump()
        {
            for (unsigned i = 0; i < 1000; ++i)
                if (ioContext.poll() + uiContext.poll() == 0) return;
            throw std::runtime_error("Connection did not become idle");
        }
        void drive(std::chrono::milliseconds duration)
        {
            const auto until = std::chrono::steady_clock::now() + duration;
            do { pump(); std::this_thread::sleep_for(1ms); } while (std::chrono::steady_clock::now() < until);
            pump();
        }
        void connect()
        {
            io.start(); ui.start(); pump();
            ASSERT_EQ(io.connectionStatus().state, State::Connected);
            ASSERT_EQ(ui.connectionStatus().state, State::Connected);
        }
        void TearDown() override { endpoints[0]->close(); pump(); io.stop(); ui.stop(); }
    };

    TEST_F(ConnectionTest, ValidatesOptionsAndReportsInitialStateWithoutInvokingCallback)
    {
        const p2p::ConnectionOptions defaults;
        EXPECT_EQ(defaults.heartbeat, 100ms); EXPECT_EQ(defaults.timeout, 500ms);
        EXPECT_EQ(io.connectionStatus(), p2p::ConnectionStatus{});
        EXPECT_TRUE(ioEvents.empty());
        EXPECT_THROW((p2p::Bridge{ioBus, ioContext, ioTransport, {0ms, 500ms}}), std::invalid_argument);
        EXPECT_THROW((p2p::Bridge{ioBus, ioContext, ioTransport, {100ms, 100ms}}), std::invalid_argument);
    }

    TEST_F(ConnectionTest, QueuedCommandsWaitForThePeerHandshake)
    {
        auto provider = ioBus.service<^^Services::normal>().serve([](auto value) { return value; });
        ui.start(); drain(uiContext);
        auto call = uiBus.service<^^Services::normal>().request(42, 1s);
        drain(uiContext);
        EXPECT_EQ(ui.connectionStatus().state, State::Connecting);
        EXPECT_EQ(provider.poll(), 0U); EXPECT_FALSE(call.ready());
        io.start(); pump();
        ASSERT_EQ(provider.poll(), 1U); pump();
        ASSERT_TRUE(call.ready()); EXPECT_EQ(call.get(), 42U);
        EXPECT_EQ(uiEvents, (std::vector<p2p::ConnectionStatus>{{State::Connecting}, {State::Connected}}));
        EXPECT_THROW(ui.setConnectionCallback({}), std::logic_error);
    }

    TEST_F(ConnectionTest, HealthyIdlePeersExchangeAcknowledgedHeartbeatsWithoutExtraStateEvents)
    {
        connect(); drive(300ms);
        EXPECT_EQ(io.connectionStatus().state, State::Connected);
        EXPECT_EQ(ui.connectionStatus().state, State::Connected);
        EXPECT_GE(ioTransport.heartbeats, 5U); EXPECT_GE(uiTransport.heartbeats, 5U);
        EXPECT_GE(ioTransport.acknowledgements, 5U); EXPECT_GE(uiTransport.acknowledgements, 5U);
        EXPECT_EQ(ioEvents.size(), 2U); EXPECT_EQ(uiEvents.size(), 2U);
        EXPECT_FALSE(io.failure()); EXPECT_FALSE(ui.failure());
    }

    TEST_F(ConnectionTest, AbsentPeerTimesOutAndFailsQueuedWork)
    {
        ui.start();
        auto call = uiBus.service<^^Services::normal>().request(1, 1s);
        // No IO bridge is started. The open transport alone is not readiness.
        drive(180ms);
        EXPECT_EQ(ui.connectionStatus(), (p2p::ConnectionStatus{State::Failed, Reason::HandshakeTimeout}));
        EXPECT_TRUE(ui.failure());
        ASSERT_TRUE(call.ready()); EXPECT_EQ(call.get(), std::unexpected(pnm::msg::ServiceError::TransportError));
        EXPECT_EQ(uiEvents.size(), 2U);
        EXPECT_THROW(ui.start(), std::logic_error);
    }

    TEST_F(ConnectionTest, SilentPeerFailsAcceptedActionWithoutClaimingTheMotorStopped)
    {
        connect();
        std::optional<Execution> execution;
        auto provider = ioBus.action<^^Actions::move>().serveDeferred([&](auto, Execution value) {
            value.accept(); execution.emplace(std::move(value));
        });
        std::optional<pnm::msg::ActionResult<unsigned>> result;
        auto goal = uiBus.action<^^Actions::move>().sendGoal(1, {.accept_timeout = 1s}, {
          .on_result = [&](auto value) { result.emplace(std::move(value)); }
        });
        pump(); ASSERT_EQ(provider.poll(), 1U); pump(); goal.poll(); ASSERT_TRUE(execution);
        // IO remains responsive; its remote UI processor stops servicing messages.
        const auto until = std::chrono::steady_clock::now() + 180ms;
        while (std::chrono::steady_clock::now() < until) { drain(ioContext); std::this_thread::sleep_for(1ms); }
        drain(ioContext);
        EXPECT_EQ(io.connectionStatus(), (p2p::ConnectionStatus{State::Failed, Reason::HeartbeatTimeout}));
        EXPECT_TRUE(execution->cancelRequested()); EXPECT_TRUE(execution->pending());
        pump(); ASSERT_TRUE(goal.poll()); ASSERT_TRUE(result); EXPECT_FALSE(*result);
        EXPECT_EQ(result->error(), pnm::msg::ActionError::TransportError);
        execution->cancelled(0); // The application still owns physical cleanup.
        drive(40ms);
        EXPECT_EQ(ioEvents.back().state, State::Failed);
        EXPECT_EQ(ioEvents.size(), 3U); // Resuming the old peer never reconnects.
    }

    TEST_F(ConnectionTest, HeartbeatsRemainHealthyWhileNormalSenderIsBlocked)
    {
        connect(); uiTransport.blocked = Lane::Normal;
        auto call = uiBus.service<^^Services::normal>().request(1, 1s);
        pump(); ASSERT_TRUE(uiTransport.waiting);
        drive(300ms);
        EXPECT_EQ(io.connectionStatus().state, State::Connected);
        EXPECT_EQ(ui.connectionStatus().state, State::Connected);
        EXPECT_FALSE(call.ready()); EXPECT_TRUE(uiTransport.waiting);
    }

    TEST_F(ConnectionTest, DeadlineStillRunsWhenTheUrgentSenderIsBlocked)
    {
        connect(); uiTransport.blocked = Lane::Urgent;
        auto call = uiBus.service<^^Services::urgent>().request(1, 1s);
        pump(); ASSERT_TRUE(uiTransport.waiting);
        drive(180ms);
        EXPECT_EQ(ui.connectionStatus(), (p2p::ConnectionStatus{State::Failed, Reason::HeartbeatTimeout}));
        EXPECT_FALSE(uiTransport.waiting); // Timeout cancelled the borrowed transport operation.
        ASSERT_TRUE(call.ready()); EXPECT_EQ(call.get(), std::unexpected(pnm::msg::ServiceError::TransportError));
    }

    TEST_F(ConnectionTest, IncomingHeartbeatsCannotHideLostAcknowledgements)
    {
        connect(); uiTransport.dropAcknowledgements = true;
        drive(180ms);
        EXPECT_GT(uiTransport.heartbeats, 0U); EXPECT_GT(uiTransport.acknowledgements, 0U);
        EXPECT_EQ(io.connectionStatus(), (p2p::ConnectionStatus{State::Failed, Reason::HeartbeatTimeout}));
    }

    TEST_F(ConnectionTest, ExplicitDisconnectNotifiesFailureAndStopsAllWaiters)
    {
        connect(); endpoints[0]->close(); pump();
        EXPECT_EQ(io.connectionStatus(), (p2p::ConnectionStatus{State::Failed, Reason::TransportError}));
        EXPECT_EQ(ui.connectionStatus(), (p2p::ConnectionStatus{State::Failed, Reason::TransportError}));
        io.stop(); ui.stop(); EXPECT_EQ(ioEvents.size(), 3U); EXPECT_EQ(uiEvents.size(), 3U);
    }

    TEST_F(ConnectionTest, GracefulStopNotifiesBothPeersAndRetiresTimers)
    {
        connect(); std::atomic_bool stopped{};
        std::jthread stopper([&] { io.stop(); stopped = true; });
        while (!stopped) { pump(); std::this_thread::sleep_for(1ms); }
        stopper.join(); pump(); ui.stop();
        EXPECT_EQ(io.connectionStatus(), (p2p::ConnectionStatus{State::Stopped}));
        EXPECT_EQ(ui.connectionStatus(), (p2p::ConnectionStatus{State::Stopped, Reason::PeerStopped}));
        EXPECT_EQ(ioEvents, (std::vector<p2p::ConnectionStatus>{{State::Connecting}, {State::Connected}, {State::Stopping}, {State::Stopped}}));
        EXPECT_FALSE(io.failure()); EXPECT_FALSE(ui.failure());
        const auto eventCount = ioEvents.size() + uiEvents.size();
        drive(180ms);
        EXPECT_EQ(ioEvents.size() + uiEvents.size(), eventCount);
        EXPECT_EQ(ioContext.poll() + uiContext.poll(), 0U);
    }

    TEST_F(ConnectionTest, ResumingAStalledContextDoesNotExecuteBufferedCommands)
    {
        connect();
        unsigned executed{};
        auto provider = ioBus.service<^^Services::normal>().serve([&](auto value) { ++executed; return value; });
        auto call = uiBus.service<^^Services::normal>().request(42, 1s);
        drain(uiContext); // Request is now in the peer's normal queue.
        std::this_thread::sleep_for(160ms);
        pump();
        EXPECT_EQ(provider.poll(), 0U); EXPECT_EQ(executed, 0U);
        ASSERT_TRUE(call.ready()); EXPECT_EQ(call.get(), std::unexpected(pnm::msg::ServiceError::TransportError));
        EXPECT_EQ(io.connectionStatus().reason, Reason::HeartbeatTimeout);
        auto afterFailure = uiBus.service<^^Services::normal>().request(43, 1s);
        EXPECT_EQ(afterFailure.get(), std::unexpected(pnm::msg::ServiceError::Unavailable));
    }

    TEST_F(ConnectionTest, StopDuringHandshakeCancelsQueuedCallsAndReportsStopped)
    {
        ui.start(); drain(uiContext);
        auto call = uiBus.service<^^Services::normal>().request(42, 1s);
        std::atomic_bool stopped{};
        std::jthread stopper([&] { ui.stop(); stopped = true; });
        while (!stopped) { pump(); std::this_thread::sleep_for(1ms); }
        stopper.join();
        EXPECT_FALSE(ui.failure());
        EXPECT_EQ(uiEvents, (std::vector<p2p::ConnectionStatus>{{State::Connecting}, {State::Stopping}, {State::Stopped}}));
        ASSERT_TRUE(call.ready()); EXPECT_EQ(call.get(), std::unexpected(pnm::msg::ServiceError::Cancelled));
    }

    TEST_F(ConnectionTest, ThrowingFinalObserverCannotLeaveStopWaiting)
    {
        io.setConnectionCallback([](auto status) {
            if (status.state == State::Stopped || status.state == State::Failed)
                throw std::runtime_error("Observer failed during teardown");
        });
        connect(); std::atomic_bool stopped{};
        std::jthread stopper([&] { io.stop(); stopped = true; });
        while (!stopped) { pump(); std::this_thread::sleep_for(1ms); }
        stopper.join();
        EXPECT_EQ(io.connectionStatus(), (p2p::ConnectionStatus{State::Failed, Reason::InternalError}));
        EXPECT_TRUE(io.failure());
    }

    TEST(ConnectionThreads, NotificationsRunOnContextAndFinishBeforeStopReturns)
    {
        pnm::msg::Bus leftBus, rightBus;
        pnm::coro::Context context;
        auto endpoints = p2p::InProcessTransport::pair();
        auto runner = example::run_context(context);
        const auto caller = std::this_thread::get_id();
        std::mutex mutex;
        std::condition_variable changed;
        bool connected{}, wrongThread{};
        unsigned callbacks{};
        p2p::Bridge left(leftBus, context, *endpoints[0]), right(rightBus, context, *endpoints[1]);
        left.setConnectionCallback([&](auto status) {
            std::scoped_lock lock(mutex);
            EXPECT_EQ(left.connectionStatus(), status); // Reentrant snapshot access.
            wrongThread |= std::this_thread::get_id() == caller;
            ++callbacks;
            if (status.state == State::Connected) connected = true;
            changed.notify_one();
        });
        left.start(); right.start();
        {
            std::unique_lock lock(mutex);
            EXPECT_TRUE(changed.wait_for(lock, 2s, [&] { return connected; }));
        }
        left.stop(); right.stop();
        std::scoped_lock lock(mutex);
        EXPECT_FALSE(wrongThread); EXPECT_EQ(callbacks, 4U);
        EXPECT_FALSE(left.failure()); EXPECT_FALSE(right.failure());
    }

    TEST_F(ConnectionTest, ThrowingObserverFailsTheLinkWithoutPreventingTeardown)
    {
        io.setConnectionCallback([this](auto status) {
            ioEvents.push_back(status);
            if (status.state == State::Connected) throw std::invalid_argument("Observer failed");
        });
        io.start(); ui.start(); pump();
        EXPECT_EQ(io.connectionStatus(), (p2p::ConnectionStatus{State::Failed, Reason::InternalError}));
        EXPECT_TRUE(io.failure()); io.stop();
        EXPECT_EQ(ioEvents.size(), 3U);
    }

    template<typename T>
    pnm::coro::Task<void> capture(pnm::coro::Task<T> task, std::shared_ptr<std::optional<T>> result)
    { result->emplace(co_await task); }

    // An independent peer, to test wire sequencing without mirroring the health implementation.
    std::array<std::byte, 16> control(unsigned kind, std::uint64_t id = 0)
    {
        std::array<std::byte, 16> bytes{};
        bytes[0] = std::byte{6}; bytes[1] = static_cast<std::byte>(kind);
        for (unsigned i = 0; i < 8; ++i) bytes[4 + i] = static_cast<std::byte>(id >> (8 * i));
        return bytes;
    }
    class ConnectionWireTest : public ::testing::Test
    {
      protected:
        pnm::msg::Bus bus;
        pnm::coro::Context context;
        std::array<std::shared_ptr<p2p::InProcessTransport>, 2> endpoints{p2p::InProcessTransport::pair()};
        p2p::Bridge bridge{bus, context, *endpoints[0], {20ms, 120ms}};
        void drain() { while (context.poll()) {} }
        template<typename T> T run(pnm::coro::Task<T> task)
        {
            auto result = std::make_shared<std::optional<T>>();
            pnm::coro::co_spawn(context, capture(std::move(task), result)); drain();
            if (!*result) throw std::runtime_error("Expected completed transport operation");
            return std::move(**result);
        }
        void send(unsigned kind, std::uint64_t id = 0, Lane lane = Lane::Urgent)
        { const auto bytes = control(kind, id); ASSERT_TRUE(run(endpoints[1]->send(bytes, lane))); }
        auto receive()
        {
            std::array<std::byte, 16> bytes{};
            const auto result = run(endpoints[1]->receive(bytes, Lane::Urgent));
            if (!result || *result != bytes.size()) throw std::runtime_error("Invalid control size");
            return bytes;
        }
        void connect()
        {
            bridge.start(); drain(); EXPECT_EQ(receive(), control(13));
            send(13); EXPECT_EQ(receive(), control(14)); send(14);
            ASSERT_EQ(bridge.connectionStatus().state, State::Connected);
        }
        void TearDown() override { endpoints[0]->close(); drain(); bridge.stop(); }
    };

    TEST_F(ConnectionWireTest, ReplayedAcknowledgementsCannotRefreshTheDeadline)
    {
        connect(); std::this_thread::sleep_for(30ms); drain();
        EXPECT_EQ(receive(), control(15, 1)); send(16, 1);
        for (unsigned i = 0; i < 5; ++i) {
            std::this_thread::sleep_for(30ms); send(16, 1);
        }
        EXPECT_EQ(bridge.connectionStatus(), (p2p::ConnectionStatus{State::Failed, Reason::HeartbeatTimeout}));
    }

    TEST_F(ConnectionWireTest, PeerGoodbyeWithAcceptedWorkReportsUnknownOutcome)
    {
        bridge.registerActions<Actions>(Provider::UI);
        connect();
        std::optional<pnm::msg::ActionResult<unsigned>> result;
        auto goal = bus.action<^^Actions::move>().sendGoal(1, {.accept_timeout = 1s}, {
          .on_result = [&](auto value) { result.emplace(std::move(value)); }
        });
        drain();
        std::array<std::byte, 64> request{};
        ASSERT_TRUE(run(endpoints[1]->receive(request, Lane::Normal)));
        send(7, 1, Lane::Normal); goal.poll(); // Peer accepted the action.
        send(17);
        EXPECT_EQ(bridge.connectionStatus(), (p2p::ConnectionStatus{State::Failed, Reason::PeerStopped}));
        ASSERT_TRUE(goal.poll()); ASSERT_TRUE(result); EXPECT_FALSE(*result);
        EXPECT_EQ(result->error(), pnm::msg::ActionError::TransportError);
    }

    TEST_F(ConnectionWireTest, ReadyBeforeHelloIsProtocolFailure)
    {
        bridge.start(); drain(); receive(); send(14);
        EXPECT_EQ(bridge.connectionStatus(), (p2p::ConnectionStatus{State::Failed, Reason::ProtocolError}));
    }

    TEST_F(ConnectionWireTest, NewHelloRequiresExplicitRecovery)
    {
        connect(); send(13);
        EXPECT_EQ(bridge.connectionStatus(), (p2p::ConnectionStatus{State::Failed, Reason::PeerRestarted}));
    }

    TEST_F(ConnectionWireTest, UnknownAcknowledgementFailsProtocol)
    {
        connect(); send(16, 123);
        EXPECT_EQ(bridge.connectionStatus(), (p2p::ConnectionStatus{State::Failed, Reason::ProtocolError}));
    }

    TEST_F(ConnectionWireTest, HealthFramesMustUseUrgentLane)
    {
        connect(); send(15, 1, Lane::Normal);
        EXPECT_EQ(bridge.connectionStatus(), (p2p::ConnectionStatus{State::Failed, Reason::ProtocolError}));
    }
}
