#include "Actions.hpp"
#include "ContextThread.hpp"
#include "common/icc/bridge/Bridge.hpp"
#include "common/icc/transport/InProcessTransport.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace
{
    using namespace std::chrono_literals;
    using Execution = pnm::msg::ActionExecution<std::uint32_t, std::uint32_t>;
    constexpr std::uint32_t TARGET{ 100 };
    constexpr std::uint32_t PROGRESS{ 25 };
    // A coalesced worker notification. Readiness callbacks retain it by shared_ptr
    // so a callback already running during unregistration cannot use freed storage.
    class Signal
    {
      public:
        void notify()
        {
            std::scoped_lock lock(m_mutex);
            m_pending = true;
            m_changed.notify_one();
        }
        void wait()
        {
            std::unique_lock lock(m_mutex);
            if (!m_changed.wait_for(lock, 2s, [this] { return m_pending; })) {
                throw std::runtime_error("Timed out waiting for action readiness");
            }
            m_pending = false;
        }

      private:
        std::mutex m_mutex;
        std::condition_variable m_changed;
        bool m_pending{};
    };

    void run_action(pnm::msg::Bus& ui_bus,
                    auto& provider,
                    std::optional<Execution>& execution,
                    const std::uint32_t& target,
                    const std::shared_ptr<Signal>& io_ready,
                    bool cancel)
    {
        using example::Actions;
        auto ui_ready = std::make_shared<Signal>();
        bool accepted{};
        std::uint32_t current{};
        std::optional<pnm::msg::ActionResult<std::uint32_t>> result;
        auto goal = ui_bus.action<^^Actions::demo_io_count>().sendGoal(
          TARGET, { .accept_timeout = 2s }, { .on_accepted = [&] {
            accepted = true;
            std::cout << "UI: IO accepted\n";
        }, .on_feedback = [&](const std::uint32_t& count) {
            current = count;
            std::cout << "UI: count " << count << '\n';
        }, .on_result = [&](auto value) { result.emplace(std::move(value)); } });
        auto goal_readiness = pnm::msg::watch(goal, [ui_ready] { ui_ready->notify(); });
        while (!execution) {
            provider.poll();
            if (!execution) {
                io_ready->wait();
            }
        }
        while (!accepted) {
            if (goal.poll() && !accepted) {
                throw std::runtime_error("Goal was not accepted");
            }
            if (!accepted) {
                ui_ready->wait();
            }
        }
        execution->feedback(PROGRESS);
        while (current != PROGRESS) {
            if (goal.poll()) {
                throw std::runtime_error("Goal ended before feedback");
            }
            if (current != PROGRESS) {
                ui_ready->wait();
            }
        }
        if (cancel) {
            auto cancellation_readiness = pnm::msg::watch(*execution, [io_ready] { io_ready->notify(); });
            goal.requestCancel();
            while (!execution->cancelRequested()) {
                io_ready->wait();
            }
            std::cout << "IO: cleanup finished\n";
            execution->cancelled(current);
        }
        else {
            execution->succeed(target);
        }
        execution.reset();
        while (!goal.poll()) {
            ui_ready->wait();
        }
        const auto expected = cancel ? pnm::msg::ActionStatus::Cancelled : pnm::msg::ActionStatus::Succeeded;
        if (!result || !*result || (**result).status != expected ||
            (**result).value != (cancel ? PROGRESS : TARGET)) {
            throw std::runtime_error("Unexpected terminal action result");
        }
        std::cout << "UI: " << (cancel ? "cancelled" : "succeeded") << ", count " << (**result).value << '\n';
    }
}

int main()
{
    using example::ActionProvider;
    using example::Actions;
    try {
        pnm::msg::Bus io_bus;
        pnm::msg::Bus ui_bus;
        pnm::coro::Context context;
        auto endpoints = icc::InProcessTransport::pair();
        auto icc_thread = example::run_context(context);
        icc::Bridge io_bridge(io_bus, context, *endpoints[0]);
        icc::Bridge ui_bridge(ui_bus, context, *endpoints[1]);
        io_bridge.registerActions<Actions>(ActionProvider::IO);
        ui_bridge.registerActions<Actions>(ActionProvider::UI);

        auto io_ready = std::make_shared<Signal>();
        std::optional<Execution> execution;
        std::uint32_t target{};
        auto provider = io_bus.action<^^Actions::demo_io_count>().serveDeferred(
          [&](std::uint32_t requested, Execution next) {
            if (execution) {
                next.fail(pnm::msg::ActionError::Busy);
                return;
            }
            if (requested == 0) {
                next.reject();
                return;
            }
            if (next.cancelRequested()) {
                next.cancelled(0);
                return;
            }
            if (!next.accept()) {
                return;
            }
            target = requested;
            execution.emplace(std::move(next));
        });
        auto provider_readiness = pnm::msg::watch(provider, [io_ready] { io_ready->notify(); });
        io_bridge.start();
        ui_bridge.start();

        // Main represents application workers in this demonstration. It dispatches
        // IO admission and UI callbacks; the dedicated icc thread only forwards.
        for (bool cancel : { false, true }) {
            run_action(ui_bus, provider, execution, target, io_ready, cancel);
        }

        io_bridge.stop();
        ui_bridge.stop();
        if (io_bridge.failure() || ui_bridge.failure()) {
            throw std::runtime_error("Bridge failed");
        }
        std::cout << "Action completion and cancellation verified.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "icc action example: " << error.what() << '\n';
        return 1;
    }
}
