#include "../examples/ContextThread.hpp"
#include "common/icc/bridge/Bridge.hpp"
#include "common/icc/transport/InProcessTransport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using namespace std::chrono_literals;
    using namespace pnm::meta::string::literals;
    using Error = pnm::msg::ActionError;
    using Status = pnm::msg::ActionStatus;
    using Execution = pnm::msg::ActionExecution<std::uint32_t, std::uint32_t>;
    enum class Provider
    {
        IO = 10,
        UI = 20
    };
    struct Actions
    {
        [[= Provider::IO]] pnm::msg::Action<std::uint32_t, std::uint32_t, std::uint32_t> io_count;
        [[ = "ui/count"_fs,
           = 42,
           = Provider::UI ]] pnm::msg::Action<std::uint32_t, std::uint32_t, std::uint32_t>
          reverse;
        [[= Provider::IO]] pnm::msg::Action<std::string, std::string, std::string> io_text;
        [[= Provider::IO]] pnm::msg::Action<std::uint32_t, std::string, std::uint64_t> io_mixed;
        pnm::msg::Action<std::uint32_t, std::uint32_t, std::uint32_t> local;
        int ignored;
    };
    static_assert(std::string_view(pnm::msg::ActionMetadata<^^Actions::io_count>::name) == "io/count");
    static_assert(std::string_view(pnm::msg::ActionMetadata<^^Actions::reverse>::name) == "ui/count");

    template<typename T = std::uint32_t>
    struct Observer
    {
        std::vector<char> events;
        std::vector<T> feedback;
        std::optional<pnm::msg::ActionResult<T>> result;
        pnm::msg::ActionCallbacks<T, T> callbacks()
        {
            return {
                .on_accepted = [this] { events.push_back('a'); },
                .on_feedback =
                  [this](const T& value) {
                events.push_back('f');
                feedback.push_back(value);
            },
                .on_result =
                  [this](pnm::msg::ActionResult<T> value) {
                events.push_back('r');
                result.emplace(std::move(value));
            },
            };
        }
    };

    class ActionTest : public ::testing::Test
    {
      protected:
        pnm::msg::Bus ioBus, uiBus;
        pnm::coro::Context ioContext, uiContext;
        std::array<std::shared_ptr<icc::InProcessTransport>, 2> endpoints{ icc::InProcessTransport::pair() };
        icc::Bridge io{ ioBus, ioContext, *endpoints[0] }, ui{ uiBus, uiContext, *endpoints[1] };
        void SetUp() override
        {
            io.registerActions<Actions>(Provider::IO);
            ui.registerActions<Actions>(Provider::UI);
            io.start();
            ui.start();
        }
        void drain()
        {
            for (unsigned i = 0; i < 1000; ++i)
                if (ioContext.poll() + uiContext.poll() == 0)
                    return;
            FAIL() << "Bridge did not become idle";
        }
        auto submit(Observer<>& observer, std::chrono::milliseconds timeout = 2s)
        {
            return uiBus.action<^^Actions::io_count>().sendGoal(
              100, { .accept_timeout = timeout }, observer.callbacks());
        }
        void TearDown() override
        {
            endpoints[0]->close();
            drain();
            io.stop();
            ui.stop();
        }
    };

    TEST_F(ActionTest, RemoteAcceptancePrecedesLocalAcceptanceAndCallbacksStayOnCaller)
    {
        std::optional<Execution> execution;
        auto provider = ioBus.action<^^Actions::io_count>().serveDeferred(
          [&](auto, Execution value) { execution.emplace(std::move(value)); });
        Observer observer;
        auto goal = submit(observer);
        drain();
        EXPECT_TRUE(observer.events.empty());
        EXPECT_EQ(provider.poll(), 1U);
        ASSERT_TRUE(execution);
        drain();
        goal.poll();
        EXPECT_TRUE(observer.events.empty());
        ASSERT_TRUE(execution->accept());
        drain();
        EXPECT_TRUE(observer.events.empty()); // Bridge does not invoke UI callbacks.
        goal.poll();
        EXPECT_EQ(observer.events, std::vector<char>{ 'a' });
        execution->feedback(10);
        execution->feedback(20);
        execution->feedback(30);
        execution->succeed(100);
        drain();
        ASSERT_TRUE(goal.poll());
        EXPECT_EQ(observer.events, (std::vector<char>{ 'a', 'f', 'r' }));
        EXPECT_EQ(observer.feedback, std::vector<std::uint32_t>{ 30 });
        ASSERT_TRUE(observer.result && *observer.result);
        EXPECT_EQ((**observer.result).status, Status::Succeeded);
        EXPECT_EQ((**observer.result).value, 100U);
        EXPECT_FALSE(io.failure());
        EXPECT_FALSE(ui.failure());
    }

    TEST_F(ActionTest, CatalogRoutesBothDirectionsAndLeavesUnannotatedActionLocal)
    {
        auto left = ioBus.action<^^Actions::io_count>().serveDeferred([](auto n, Execution e) {
            e.accept();
            e.succeed(n);
        });
        auto right = uiBus.action<^^Actions::reverse>().serveDeferred([](auto n, Execution e) {
            e.accept();
            e.succeed(n + 1);
        });
        auto local = ioBus.action<^^Actions::local>().serveDeferred([](auto n, Execution e) {
            e.accept();
            e.succeed(n);
        });
        Observer a, b, c;
        auto first = submit(a);
        auto second = ioBus.action<^^Actions::reverse>().sendGoal(5, {}, b.callbacks());
        auto missing = uiBus.action<^^Actions::local>().sendGoal(1, {}, c.callbacks());
        ASSERT_TRUE(missing.poll());
        EXPECT_EQ(c.result->error(), Error::Unavailable);
        drain();
        EXPECT_EQ(left.poll(), 1U);
        EXPECT_EQ(right.poll(), 1U);
        EXPECT_EQ(local.poll(), 0U);
        drain();
        ASSERT_TRUE(first.poll());
        ASSERT_TRUE(second.poll());
        EXPECT_EQ((**a.result).value, 100U);
        EXPECT_EQ((**b.result).value, 6U);
    }

    TEST_F(ActionTest, PreservesDistinctGoalFeedbackAndResultTypes)
    {
        auto provider = ioBus.action<^^Actions::io_mixed>().serveDeferred(
          [](std::uint32_t n, pnm::msg::ActionExecution<std::string, std::uint64_t> e) {
            e.accept();
            e.feedback("working");
            e.succeed(std::uint64_t{ n } << 40);
        });
        std::string feedback;
        std::optional<pnm::msg::ActionResult<std::uint64_t>> result;
        auto goal = uiBus.action<^^Actions::io_mixed>().sendGoal(
          42, {}, { .on_feedback = [&](const std::string& value) {
            feedback = value;
        }, .on_result = [&](auto value) { result.emplace(std::move(value)); } });
        drain();
        provider.poll();
        drain();
        ASSERT_TRUE(goal.poll());
        EXPECT_EQ(feedback, "working");
        ASSERT_TRUE(result && *result);
        EXPECT_EQ((**result).value, UINT64_C(42) << 40);
    }

    TEST_F(ActionTest, RejectsBeforeAcceptanceWithOrWithoutPayload)
    {
        unsigned count{};
        auto provider = ioBus.action<^^Actions::io_count>().serveDeferred([&](auto, Execution e) {
            if (count++ == 0)
                e.reject(42);
            else
                e.reject();
        });
        Observer a, b;
        auto first = submit(a);
        auto second = submit(b);
        drain();
        EXPECT_EQ(provider.poll(), 2U);
        drain();
        ASSERT_TRUE(first.poll());
        ASSERT_TRUE(second.poll());
        EXPECT_EQ(a.events, std::vector<char>{ 'r' });
        EXPECT_EQ((**a.result).status, Status::Rejected);
        EXPECT_EQ((**a.result).value, 42U);
        EXPECT_EQ(b.result->error(), Error::Rejected);
    }

    TEST_F(ActionTest, ReportsUnavailableAndHandlerFailure)
    {
        Observer a, b;
        auto first = submit(a);
        drain();
        ASSERT_TRUE(first.poll());
        EXPECT_EQ(a.result->error(), Error::Unavailable);
        auto provider = ioBus.action<^^Actions::io_count>().serveDeferred(
          [](auto, Execution) { throw std::runtime_error("failed"); });
        auto second = submit(b);
        drain();
        provider.poll();
        drain();
        ASSERT_TRUE(second.poll());
        EXPECT_EQ(b.result->error(), Error::HandlerFailed);
        EXPECT_FALSE(io.failure());
        EXPECT_FALSE(ui.failure());
    }

    TEST_F(ActionTest, CancellationBeforeForwardingDoesNotReachProvider)
    {
        auto provider = ioBus.action<^^Actions::io_count>().serveDeferred(
          [](auto, Execution) { FAIL() << "Unexpected execution"; });
        Observer observer;
        auto goal = submit(observer);
        goal.requestCancel();
        drain();
        EXPECT_EQ(provider.poll(), 0U);
        ASSERT_TRUE(goal.poll());
        EXPECT_EQ((**observer.result).status, Status::Cancelled);
    }

    TEST_F(ActionTest, CancellationWaitsForCleanupAndCanPrecedeAcceptance)
    {
        std::optional<Execution> execution;
        auto provider = ioBus.action<^^Actions::io_count>().serveDeferred(
          [&](auto, Execution e) { execution.emplace(std::move(e)); });
        for (bool accept : { false, true }) {
            Observer observer;
            auto goal = submit(observer);
            drain();
            provider.poll();
            if (accept)
                execution->accept();
            drain();
            goal.poll();
            goal.requestCancel();
            drain();
            ASSERT_TRUE(execution->cancelRequested());
            EXPECT_FALSE(goal.ready());
            execution->cancelled(17);
            execution.reset();
            drain();
            ASSERT_TRUE(goal.poll());
            EXPECT_EQ((**observer.result).status, Status::Cancelled);
            EXPECT_EQ((**observer.result).value, 17U);
        }
    }

    TEST_F(ActionTest, CompletionMayWinCancellationRaceAndTerminalCallbackRunsOnce)
    {
        std::optional<Execution> execution;
        auto provider = ioBus.action<^^Actions::io_count>().serveDeferred([&](auto, Execution e) {
            e.accept();
            execution.emplace(std::move(e));
        });
        Observer observer;
        auto goal = submit(observer);
        drain();
        provider.poll();
        drain();
        goal.poll();
        goal.requestCancel();
        ASSERT_TRUE(execution->succeed(90));
        drain();
        EXPECT_TRUE(goal.poll());
        EXPECT_TRUE(goal.poll());
        EXPECT_EQ((**observer.result).status, Status::Succeeded);
        EXPECT_EQ(observer.events, (std::vector<char>{ 'a', 'r' }));
    }

    TEST_F(ActionTest, DestroyingCallerRequestsCancellationWithoutDestroyingProviderToken)
    {
        std::optional<Execution> execution;
        auto provider = ioBus.action<^^Actions::io_count>().serveDeferred([&](auto, Execution e) {
            e.accept();
            execution.emplace(std::move(e));
        });
        Observer observer;
        {
            auto goal = submit(observer);
            drain();
            provider.poll();
            drain();
        }
        drain();
        EXPECT_TRUE(execution->pending());
        EXPECT_TRUE(execution->cancelRequested());
        execution->cancelled(0);
        execution.reset();
        drain();
        EXPECT_TRUE(observer.events.empty());
        EXPECT_FALSE(io.failure());
        EXPECT_FALSE(ui.failure());
    }

    TEST_F(ActionTest, AcceptedActionOutlivesAdmissionTimeout)
    {
        std::optional<Execution> execution;
        auto provider = ioBus.action<^^Actions::io_count>().serveDeferred([&](auto, Execution e) {
            e.accept();
            execution.emplace(std::move(e));
        });
        Observer observer;
        auto goal = submit(observer, 100ms);
        drain();
        provider.poll();
        drain();
        std::this_thread::sleep_for(120ms);
        drain();
        EXPECT_FALSE(goal.ready());
        execution->abort(9);
        drain();
        ASSERT_TRUE(goal.poll());
        EXPECT_EQ((**observer.result).status, Status::Aborted);
    }

    TEST_F(ActionTest, AcceptanceTimeoutCancelsPeerAndLateResultReleasesCapacity)
    {
        std::optional<Execution> execution;
        auto provider = ioBus.action<^^Actions::io_count>().serveDeferred(
          [&](auto, Execution e) { execution.emplace(std::move(e)); });
        Observer observer;
        auto goal = submit(observer, 30ms);
        drain();
        provider.poll();
        std::this_thread::sleep_for(40ms);
        drain();
        ASSERT_TRUE(goal.poll());
        EXPECT_EQ(observer.result->error(), Error::Timeout);
        EXPECT_TRUE(execution->cancelRequested());
        execution.reset();
        drain();
        EXPECT_FALSE(io.failure());
        EXPECT_FALSE(ui.failure());
        Observer next;
        auto another = submit(next);
        drain();
        provider.poll();
        execution->accept();
        execution->succeed(1);
        drain();
        ASSERT_TRUE(another.poll());
        EXPECT_EQ((**next.result).value, 1U);
    }

    TEST_F(ActionTest, BoundsGoalsAndReusesCapacityAfterAcknowledgement)
    {
        std::vector<Execution> executions;
        auto provider = ioBus.action<^^Actions::io_count>().serveDeferred([&](auto, Execution e) {
            e.accept();
            executions.push_back(std::move(e));
        });
        std::vector<Observer<>> observers(9);
        std::vector<pnm::msg::PendingGoal<std::uint32_t, std::uint32_t>> goals;
        for (unsigned i = 0; i < 8; ++i)
            goals.push_back(submit(observers[i]));
        drain();
        provider.poll();
        drain();
        auto excess = submit(observers[8]);
        drain();
        ASSERT_TRUE(excess.poll());
        EXPECT_EQ(observers[8].result->error(), Error::Busy);
        ASSERT_EQ(executions.size(), 8U);
        for (auto& e : executions)
            e.succeed(1);
        executions.clear();
        drain();
        for (auto& goal : goals)
            EXPECT_TRUE(goal.poll());
        Observer next;
        auto goal = submit(next);
        drain();
        provider.poll();
        ASSERT_EQ(executions.size(), 1U);
        executions[0].succeed(2);
        drain();
        EXPECT_TRUE(goal.poll());
        EXPECT_EQ((**next.result).value, 2U);
    }

    TEST_F(ActionTest, RetainsResultsWhileTransportIsSaturated)
    {
        std::vector<Execution> executions;
        auto provider = ioBus.action<^^Actions::io_count>().serveDeferred([&](auto, Execution e) {
            e.accept();
            executions.push_back(std::move(e));
        });
        std::vector<Observer<>> observers(8);
        std::vector<pnm::msg::PendingGoal<std::uint32_t, std::uint32_t>> goals;
        for (auto& observer : observers)
            goals.push_back(submit(observer));
        drain();
        provider.poll(); // Stop polling UI: acceptance fills the IO->UI FIFO.
        while (ioContext.poll()) {
        }
        for (unsigned i = 0; i < 8; ++i) {
            executions[i].feedback(i);
            executions[i].succeed(i + 100);
        }
        while (ioContext.poll()) {
        } // One sender suspended, all results retained.
        drain();
        for (unsigned i = 0; i < 8; ++i) {
            ASSERT_TRUE(goals[i].poll());
            ASSERT_TRUE(observers[i].result && *observers[i].result);
            EXPECT_EQ((**observers[i].result).value, i + 100);
            EXPECT_EQ(observers[i].events, (std::vector<char>{ 'a', 'f', 'r' }));
        }
        EXPECT_FALSE(io.failure());
        EXPECT_FALSE(ui.failure());
    }

    TEST_F(ActionTest, StringPayloadBoundariesAndOversizedResults)
    {
        using TextExecution = pnm::msg::ActionExecution<std::string, std::string>;
        auto provider =
          ioBus.action<^^Actions::io_text>().serveDeferred([](const auto& text, TextExecution e) {
            e.accept();
            e.feedback(text);
            e.succeed(text == "oversized" ? std::string(2048, 'x') : text);
        });
        for (const auto& text : { std::string{},
                                  std::string{ "a\0b", 3 },
                                  std::string(2048 - 16 - 7, 'x'),
                                  std::string{ "oversized" } }) {
            Observer<std::string> observer;
            auto goal = uiBus.action<^^Actions::io_text>().sendGoal(text, {}, observer.callbacks());
            drain();
            provider.poll();
            drain();
            ASSERT_TRUE(goal.poll());
            if (text == "oversized")
                EXPECT_EQ(observer.result->error(), Error::TransportError);
            else {
                ASSERT_TRUE(*observer.result);
                EXPECT_EQ((**observer.result).value, text);
            }
        }
        Observer<std::string> oversized;
        auto goal =
          uiBus.action<^^Actions::io_text>().sendGoal(std::string(2048, 'x'), {}, oversized.callbacks());
        drain();
        EXPECT_EQ(provider.poll(), 0U);
        ASSERT_TRUE(goal.poll());
        EXPECT_EQ(oversized.result->error(), Error::TransportError);
        EXPECT_FALSE(io.failure());
        EXPECT_FALSE(ui.failure());
    }

    TEST_F(ActionTest, OversizedFeedbackRequestsCleanupBeforeErrorResult)
    {
        using TextExecution = pnm::msg::ActionExecution<std::string, std::string>;
        std::optional<TextExecution> execution;
        auto provider = ioBus.action<^^Actions::io_text>().serveDeferred([&](const auto&, TextExecution e) {
            e.accept();
            execution.emplace(std::move(e));
        });
        Observer<std::string> observer;
        auto goal = uiBus.action<^^Actions::io_text>().sendGoal("start", {}, observer.callbacks());
        drain();
        provider.poll();
        drain();
        execution->feedback(std::string(2048, 'x'));
        drain();
        EXPECT_TRUE(execution->cancelRequested());
        EXPECT_FALSE(goal.ready());
        execution->cancelled("stopped");
        drain();
        ASSERT_TRUE(goal.poll());
        EXPECT_EQ(observer.result->error(), Error::TransportError);
    }

    TEST_F(ActionTest, DisconnectDoesNotReportConfirmedCancellation)
    {
        std::optional<Execution> execution;
        auto provider = ioBus.action<^^Actions::io_count>().serveDeferred([&](auto, Execution e) {
            e.accept();
            execution.emplace(std::move(e));
        });
        Observer observer;
        auto goal = submit(observer);
        drain();
        provider.poll();
        drain();
        endpoints[0]->close();
        drain();
        ASSERT_TRUE(goal.poll());
        EXPECT_EQ(observer.result->error(), Error::TransportError);
        EXPECT_TRUE(execution->cancelRequested());
        EXPECT_TRUE(execution->pending());
        EXPECT_TRUE(io.failure());
        EXPECT_TRUE(ui.failure());
    }

    TEST(ActionRegistration, ValidatesOwnerDuplicatesAndRetainsLocalEndpointCompatibility)
    {
        pnm::msg::Bus bus;
        pnm::coro::Context context;
        auto endpoints = icc::InProcessTransport::pair();
        icc::Bridge bridge(bus, context, *endpoints[0]);
        EXPECT_THROW(bridge.registerActions<Actions>(static_cast<Provider>(99)), std::invalid_argument);
        EXPECT_NO_THROW(bridge.registerActions<Actions>(Provider::IO));
        EXPECT_THROW(bridge.registerActions<Actions>(Provider::IO), std::invalid_argument);
        EXPECT_THROW((bus.action<int, int, int>("io/count")), std::invalid_argument);
        pnm::coro::Bus coroutineBus(context, bus);
        auto endpoint = coroutineBus.action<^^Actions::io_count>();
        static_assert(
          std::same_as<decltype(endpoint), pnm::coro::Action<std::uint32_t, std::uint32_t, std::uint32_t>>);
    }

    TEST(ActionRegistration, DestructionBeforeStartReleasesPendingProxyGoals)
    {
        pnm::msg::Bus bus;
        pnm::coro::Context context;
        auto endpoints = icc::InProcessTransport::pair();
        Observer observer;
        std::optional<pnm::msg::PendingGoal<std::uint32_t, std::uint32_t>> goal;
        {
            icc::Bridge bridge(bus, context, *endpoints[0]);
            bridge.registerActions<Actions>(Provider::UI);
            goal.emplace(bus.action<^^Actions::io_count>().sendGoal(1, {}, observer.callbacks()));
        }
        ASSERT_TRUE(goal->poll());
        EXPECT_EQ(observer.result->error(), Error::Unavailable);
    }
    class Signal
    {
      public:
        void notify()
        {
            std::scoped_lock lock(mutex);
            pending = true;
            changed.notify_one();
        }
        bool wait()
        {
            std::unique_lock lock(mutex);
            if (!changed.wait_for(lock, 2s, [&] { return pending; }))
                return false;
            pending = false;
            return true;
        }

      private:
        std::mutex mutex;
        std::condition_variable changed;
        bool pending{};
    };

    class ActionThreads : public ::testing::Test
    {
      protected:
        pnm::msg::Bus ioBus, uiBus;
        pnm::coro::Context ioContext, uiContext;
        std::array<std::shared_ptr<icc::InProcessTransport>, 2> endpoints{ icc::InProcessTransport::pair() };
        std::jthread ioRunner{ example::run_context(ioContext) }, uiRunner{ example::run_context(uiContext) };
        icc::Bridge io{ ioBus, ioContext, *endpoints[0] }, ui{ uiBus, uiContext, *endpoints[1] };
        void SetUp() override
        {
            io.registerActions<Actions>(Provider::IO);
            ui.registerActions<Actions>(Provider::UI);
            io.start();
            ui.start();
        }
        void TearDown() override
        {
            endpoints[0]->close();
            io.stop();
            ui.stop();
        }
    };

    TEST_F(ActionThreads, ForwardsActionsAcrossSeparateThreadsWithoutLostNotifications)
    {
        auto providerSignal = std::make_shared<Signal>();
        auto provider = ioBus.action<^^Actions::io_count>().serveDeferred([](auto n, Execution e) {
            e.accept();
            e.feedback(n - 1);
            e.succeed(n);
        });
        auto providerReady = pnm::msg::watch(provider, [providerSignal] { providerSignal->notify(); });
        for (std::uint32_t i = 1; i <= 200; ++i) {
            auto goalSignal = std::make_shared<Signal>();
            Observer observer;
            auto goal =
              uiBus.action<^^Actions::io_count>().sendGoal(i, { .accept_timeout = 2s }, observer.callbacks());
            auto goalReady = pnm::msg::watch(goal, [goalSignal] { goalSignal->notify(); });
            while (provider.poll() == 0)
                ASSERT_TRUE(providerSignal->wait());
            while (!goal.poll())
                ASSERT_TRUE(goalSignal->wait());
            ASSERT_TRUE(observer.result && *observer.result);
            EXPECT_EQ((**observer.result).value, i);
            EXPECT_EQ(observer.events, (std::vector<char>{ 'a', 'f', 'r' }));
        }
        io.stop();
        ui.stop();
        EXPECT_FALSE(io.failure());
        EXPECT_FALSE(ui.failure());
        EXPECT_THROW(io.registerActions<Actions>(Provider::IO), std::logic_error);
    }

    TEST_F(ActionThreads, GracefulStopWaitsForCleanupAndResultDelivery)
    {
        auto providerSignal = std::make_shared<Signal>();
        auto cancellationSignal = std::make_shared<Signal>();
        std::optional<Execution> execution;
        auto provider = ioBus.action<^^Actions::io_count>().serveDeferred([&](auto, Execution e) {
            e.accept();
            execution.emplace(std::move(e));
        });
        auto providerReady = pnm::msg::watch(provider, [providerSignal] { providerSignal->notify(); });
        Observer observer;
        auto goal =
          uiBus.action<^^Actions::io_count>().sendGoal(1, { .accept_timeout = 2s }, observer.callbacks());
        while (!execution) {
            provider.poll();
            if (!execution) {
                ASSERT_TRUE(providerSignal->wait());
            }
        }
        auto cancellationReady =
          pnm::msg::watch(*execution, [cancellationSignal] { cancellationSignal->notify(); });
        std::jthread stopping([&] { ui.stop(2s); });
        while (!execution->cancelRequested())
            ASSERT_TRUE(cancellationSignal->wait());
        EXPECT_FALSE(goal.ready());
        execution->cancelled(7);
        stopping.join();
        ASSERT_TRUE(goal.poll());
        ASSERT_TRUE(observer.result && *observer.result);
        EXPECT_EQ((**observer.result).status, Status::Cancelled);
        EXPECT_EQ((**observer.result).value, 7U);
        EXPECT_FALSE(ui.failure());
        io.stop();
        EXPECT_FALSE(io.failure());
    }

    TEST_F(ActionThreads, StopDeadlineFailsCallerWithoutClaimingExecutionStopped)
    {
        auto providerSignal = std::make_shared<Signal>();
        std::optional<Execution> execution;
        auto provider = ioBus.action<^^Actions::io_count>().serveDeferred([&](auto, Execution e) {
            e.accept();
            execution.emplace(std::move(e));
        });
        auto providerReady = pnm::msg::watch(provider, [providerSignal] { providerSignal->notify(); });
        Observer observer;
        auto goal =
          uiBus.action<^^Actions::io_count>().sendGoal(1, { .accept_timeout = 2s }, observer.callbacks());
        while (!execution) {
            provider.poll();
            if (!execution) {
                ASSERT_TRUE(providerSignal->wait());
            }
        }
        ui.stop(30ms);
        ASSERT_TRUE(goal.poll());
        EXPECT_TRUE(ui.failure());
        EXPECT_EQ(observer.result->error(), Error::TransportError);
        EXPECT_TRUE(execution->pending());
        EXPECT_TRUE(execution->cancelRequested());
        execution->cancelled(0); // Worker can still clean up after the caller lost confirmation.
    }

}
