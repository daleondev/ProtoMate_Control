#include "p2p/bridge/Bridge.hpp"
#include "p2p/transport/InProcessTransport.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <optional>
#include <vector>

namespace
{
    using namespace std::chrono_literals;
    using p2p::Lane;
    using Execution = pnm::msg::ActionExecution<std::uint32_t, std::uint32_t>;
    enum class Provider { IO, UI };
    struct Services
    {
        [[= Provider::IO]] pnm::msg::Service<std::uint32_t, std::uint32_t> normal;
        [[= Lane::Urgent, = Provider::IO]] pnm::msg::Service<std::uint32_t, std::uint32_t> stop;
    };
    struct Actions
    {
        [[= Provider::IO]] pnm::msg::Action<std::uint32_t, std::uint32_t, std::uint32_t> move;
    };
    struct Topics
    {
        [[= Provider::UI]] pnm::msg::Topic<std::uint32_t> state;
    };

    // Hold a normal send inside the transport indefinitely. Urgent operations
    // still use the real bounded transport, proving the bridge does not depend
    // on a normal send finishing to notice or forward urgent work.
    class GatedTransport final : public p2p::ITransport
    {
      public:
        explicit GatedTransport(ITransport& inner) : m_inner(inner) {}
        void blockNormal() { m_gate.emplace(); }
        void releaseNormal()
        {
            auto gate = std::exchange(m_gate, std::nullopt);
            if (gate) gate->close();
        }
        bool waiting{};
        pnm::coro::Task<pnm::EmptyResult<Error>> send(
          std::span<const std::byte> frame, Lane lane = Lane::Normal, std::stop_token stop = {}) override
        {
            if (lane == Lane::Normal && m_gate) {
                auto gate = *m_gate;
                waiting = true;
                co_await gate.next(stop);
                waiting = false;
            }
            co_return co_await m_inner.send(frame, lane, stop);
        }
        pnm::coro::Task<pnm::Result<std::size_t, Error>> receive(
          std::span<std::byte> buffer, Lane lane = Lane::Normal, std::stop_token stop = {}) override
        { co_return co_await m_inner.receive(buffer, lane, stop); }

      private:
        ITransport& m_inner;
        std::optional<pnm::coro::Channel<bool>> m_gate;
    };

    class UrgentTest : public ::testing::Test
    {
      protected:
        pnm::msg::Bus uiBus, ioBus;
        pnm::coro::Context uiContext, ioContext;
        std::array<std::shared_ptr<p2p::InProcessTransport>, 2> endpoints{p2p::InProcessTransport::pair()};
        GatedTransport uiTransport{*endpoints[0]};
        p2p::Bridge ui{uiBus, uiContext, uiTransport}, io{ioBus, ioContext, *endpoints[1]};

        void SetUp() override
        {
            ui.registerServices<Services>(Provider::UI); io.registerServices<Services>(Provider::IO);
            ui.registerActions<Actions>(Provider::UI); io.registerActions<Actions>(Provider::IO);
            ui.registerTopics<Topics>(Provider::UI); io.registerTopics<Topics>(Provider::IO);
            ui.start(); io.start(); pump();
        }
        static void drain(pnm::coro::Context& context) { while (context.poll()) {} }
        void pump()
        {
            for (unsigned turn = 0; turn < 10000; ++turn)
                if (uiContext.poll() + ioContext.poll() == 0) return;
            throw std::runtime_error("Bridge did not become idle");
        }
        void blockNormalSender()
        {
            uiTransport.blockNormal();
            uiBus.topic<^^Topics::state>().publish(1);
            drain(uiContext);
            ASSERT_TRUE(uiTransport.waiting);
        }
        void TearDown() override
        {
            endpoints[0]->close();
            uiTransport.releaseNormal();
            pump(); ui.stop(); io.stop();
        }
    };

    TEST_F(UrgentTest, StopServiceBypassesBlockedNormalSenderAndFullNormalCallTable)
    {
        auto normal = ioBus.service<^^Services::normal>().serve([](auto value) { return value; });
        auto stop = ioBus.service<^^Services::stop>().serve([](auto value) { return value + 1; });
        std::vector<pnm::msg::PendingCall<std::uint32_t>> calls;
        for (unsigned i = 0; i < 8; ++i) calls.push_back(uiBus.service<^^Services::normal>().request(i, 5s));
        pump(); // Normal handlers remain queued on the application's worker.
        auto excess = uiBus.service<^^Services::normal>().request(9, 5s);
        pump(); ASSERT_TRUE(excess.ready());
        EXPECT_EQ(excess.get(), std::unexpected(pnm::msg::ServiceError::Busy));
        blockNormalSender();
        auto urgent = uiBus.service<^^Services::stop>().request(41, 5s);
        pump(); ASSERT_EQ(stop.poll(), 1U); pump();
        ASSERT_TRUE(urgent.ready()); EXPECT_EQ(urgent.get(), 42U);
        EXPECT_TRUE(uiTransport.waiting);
        for (auto& call : calls) EXPECT_FALSE(call.ready());
        // Higher urgent IDs may arrive before lower normal IDs on the other lane.
        uiTransport.releaseNormal(); pump();
        EXPECT_EQ(normal.poll(), 8U); pump();
        for (unsigned i = 0; i < calls.size(); ++i) {
            ASSERT_TRUE(calls[i].ready()); EXPECT_EQ(calls[i].get(), i);
        }
        EXPECT_FALSE(ui.failure()); EXPECT_FALSE(io.failure());
    }

    TEST_F(UrgentTest, ActionCancellationAndAcknowledgementBypassBlockedNormalSend)
    {
        std::optional<Execution> execution;
        auto provider = ioBus.action<^^Actions::move>().serveDeferred([&](auto, Execution e) {
            e.accept(); execution.emplace(std::move(e));
        });
        std::optional<pnm::msg::ActionResult<std::uint32_t>> result;
        auto goal = uiBus.action<^^Actions::move>().sendGoal(1, {.accept_timeout = 5s}, {
          .on_result = [&](auto value) { result.emplace(std::move(value)); }
        });
        pump(); ASSERT_EQ(provider.poll(), 1U); pump(); goal.poll();
        ASSERT_TRUE(execution); blockNormalSender();
        goal.requestCancel(); pump();
        EXPECT_TRUE(execution->cancelRequested()); EXPECT_FALSE(goal.ready());
        execution->cancelled(42); pump(); goal.poll();
        ASSERT_TRUE(result && *result);
        EXPECT_EQ((**result).status, pnm::msg::ActionStatus::Cancelled);
        EXPECT_EQ((**result).value, 42U);
        EXPECT_TRUE(uiTransport.waiting);
        EXPECT_FALSE(ui.failure()); EXPECT_FALSE(io.failure());
    }

    TEST_F(UrgentTest, CancellationOvertakingActionGoalPreventsProviderExecution)
    {
        unsigned executed{};
        auto provider = ioBus.action<^^Actions::move>().serveDeferred([&](auto, Execution e) {
            ++executed; e.accept(); e.succeed(1);
        });
        std::optional<pnm::msg::ActionResult<std::uint32_t>> result;
        uiTransport.blockNormal();
        auto goal = uiBus.action<^^Actions::move>().sendGoal(1, {.accept_timeout = 5s}, {
          .on_result = [&](auto value) { result.emplace(std::move(value)); }
        });
        pump(); ASSERT_TRUE(uiTransport.waiting);
        goal.requestCancel(); pump();
        EXPECT_EQ(provider.poll(), 0U); EXPECT_FALSE(goal.ready());
        uiTransport.releaseNormal(); pump();
        EXPECT_EQ(provider.poll(), 0U); EXPECT_EQ(executed, 0U);
        ASSERT_TRUE(goal.poll()); ASSERT_TRUE(result && *result);
        EXPECT_EQ((**result).status, pnm::msg::ActionStatus::Cancelled);
        EXPECT_FALSE(ui.failure()); EXPECT_FALSE(io.failure());
    }

    TEST_F(UrgentTest, CancellationOvertakingServiceRequestPreventsHandlerExecution)
    {
        unsigned executed{};
        auto provider = ioBus.service<^^Services::normal>().serve([&](auto value) { ++executed; return value; });
        uiTransport.blockNormal();
        auto call = uiBus.service<^^Services::normal>().request(1, 5s);
        pump(); ASSERT_TRUE(uiTransport.waiting);
        EXPECT_TRUE(call.cancel()); pump();
        EXPECT_EQ(provider.poll(), 0U);
        uiTransport.releaseNormal(); pump();
        EXPECT_EQ(provider.poll(), 0U); EXPECT_EQ(executed, 0U);
        EXPECT_EQ(call.get(), std::unexpected(pnm::msg::ServiceError::Cancelled));
        // The cancelled operation released its reserved slot after peer response.
        auto next = uiBus.service<^^Services::normal>().request(2, 5s);
        pump(); ASSERT_EQ(provider.poll(), 1U); pump();
        ASSERT_TRUE(next.ready()); EXPECT_EQ(next.get(), 2U);
        EXPECT_FALSE(ui.failure()); EXPECT_FALSE(io.failure());
    }

    TEST_F(UrgentTest, ShutdownDeliversFinalAcknowledgementWhileNormalSenderIsBlocked)
    {
        std::optional<Execution> execution;
        auto provider = ioBus.action<^^Actions::move>().serveDeferred([&](auto, Execution e) {
            e.accept(); execution.emplace(std::move(e));
        });
        auto goal = uiBus.action<^^Actions::move>().sendGoal(1, {.accept_timeout = 5s}, {.on_result = [](auto) {}});
        pump(); ASSERT_EQ(provider.poll(), 1U); pump();
        blockNormalSender();
        std::atomic_bool uiStopped{}, ioStopped{};
        std::jthread uiStopper([&] { ui.stop(1s); uiStopped = true; });
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!uiStopped && std::chrono::steady_clock::now() < deadline) {
            pump();
            if (execution->cancelRequested()) execution->cancelled(0);
            std::this_thread::sleep_for(1ms);
        }
        EXPECT_TRUE(uiStopped);
        if (!uiStopped) { endpoints[0]->close(); uiTransport.releaseNormal(); pump(); }
        uiStopper.join();
        // IO cannot finish cleanly unless the final ACK was delivered before UI
        // stopped its urgent sender. No peer remains to rescue a dropped ACK.
        std::jthread ioStopper([&] { io.stop(1s); ioStopped = true; });
        while (!ioStopped) { pump(); std::this_thread::sleep_for(1ms); }
        ioStopper.join();
        EXPECT_FALSE(ui.failure()); EXPECT_FALSE(io.failure());
    }

    TEST_F(UrgentTest, UrgentCallCapacityIsBoundedSeparately)
    {
        auto urgentProvider = ioBus.service<^^Services::stop>().serve([](auto value) { return value; });
        auto normalProvider = ioBus.service<^^Services::normal>().serve([](auto value) { return value; });
        std::vector<pnm::msg::PendingCall<std::uint32_t>> calls;
        for (unsigned i = 0; i < 8; ++i) calls.push_back(uiBus.service<^^Services::stop>().request(i, 5s));
        pump();
        auto excess = uiBus.service<^^Services::stop>().request(9, 5s);
        auto normal = uiBus.service<^^Services::normal>().request(42, 5s);
        pump(); ASSERT_TRUE(excess.ready());
        EXPECT_EQ(excess.get(), std::unexpected(pnm::msg::ServiceError::Busy));
        ASSERT_EQ(normalProvider.poll(), 1U); pump();
        ASSERT_TRUE(normal.ready()); EXPECT_EQ(normal.get(), 42U);
        EXPECT_EQ(urgentProvider.poll(), 8U); pump();
        for (auto& call : calls) EXPECT_TRUE(call.ready());
        EXPECT_FALSE(ui.failure()); EXPECT_FALSE(io.failure());
    }
}
