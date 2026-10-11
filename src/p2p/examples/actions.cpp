#include "Actions.hpp"
#include "ContextThread.hpp"
#include "p2p/bridge/Bridge.hpp"
#include "p2p/transport/InProcessTransport.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace
{
    using namespace std::chrono_literals;
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
            if (!m_changed.wait_for(lock, 2s, [this] { return m_pending; }))
                throw std::runtime_error("Timed out waiting for action readiness");
            m_pending = false;
        }
      private:
        std::mutex m_mutex;
        std::condition_variable m_changed;
        bool m_pending{};
    };
}

int main()
{
    using example::Actions;
    using example::ActionProvider;
    using Execution = pnm::msg::ActionExecution<std::uint32_t, std::uint32_t>;
    try {
        pnm::msg::Bus io_bus, ui_bus;
        pnm::coro::Context context;
        auto endpoints = p2p::InProcessTransport::pair();
        auto p2p_thread = example::runContext(context);
        p2p::Bridge io_bridge(io_bus, context, *endpoints[0]);
        p2p::Bridge ui_bridge(ui_bus, context, *endpoints[1]);
        io_bridge.registerActions<Actions>(ActionProvider::IO);
        ui_bridge.registerActions<Actions>(ActionProvider::UI);

        auto io_ready = std::make_shared<Signal>();
        std::optional<Execution> execution;
        std::uint32_t target{};
        auto provider = io_bus.action<^^Actions::demo_io_count>().serveDeferred(
          [&](std::uint32_t requested, Execution next) {
              if (execution) { next.fail(pnm::msg::ActionError::Busy); return; }
              if (requested == 0) { next.reject(); return; }
              if (next.cancelRequested()) { next.cancelled(0); return; }
              if (!next.accept()) return;
              target = requested;
              execution.emplace(std::move(next));
          });
        auto provider_readiness = pnm::msg::watch(provider, [io_ready] { io_ready->notify(); });
        io_bridge.start(); ui_bridge.start();

        // Main represents application workers in this demonstration. It dispatches
        // IO admission and UI callbacks; the dedicated p2p thread only forwards.
        for (bool cancel : {false, true}) {
            auto ui_ready = std::make_shared<Signal>();
            bool accepted{};
            std::uint32_t current{};
            std::optional<pnm::msg::ActionResult<std::uint32_t>> result;
            auto goal = ui_bus.action<^^Actions::demo_io_count>().sendGoal(100, {.accept_timeout = 2s}, {
                .on_accepted = [&] { accepted = true; std::cout << "UI: IO accepted\n"; },
                .on_feedback = [&](const std::uint32_t& count) { current = count; std::cout << "UI: count " << count << '\n'; },
                .on_result = [&](auto value) { result.emplace(std::move(value)); }
            });
            auto goal_readiness = pnm::msg::watch(goal, [ui_ready] { ui_ready->notify(); });
            while (!execution) {
                provider.poll();
                if (!execution) io_ready->wait();
            }
            while (!accepted) {
                if (goal.poll() && !accepted) throw std::runtime_error("Goal was not accepted");
                if (!accepted) ui_ready->wait();
            }
            execution->feedback(25);
            while (current != 25) {
                if (goal.poll()) throw std::runtime_error("Goal ended before feedback");
                if (current != 25) ui_ready->wait();
            }
            if (cancel) {
                auto cancellation_readiness = pnm::msg::watch(*execution, [io_ready] { io_ready->notify(); });
                goal.requestCancel();
                while (!execution->cancelRequested()) io_ready->wait();
                std::cout << "IO: cleanup finished\n";
                execution->cancelled(current);
            } else {
                execution->succeed(target);
            }
            execution.reset();
            while (!goal.poll()) ui_ready->wait();
            const auto expected = cancel ? pnm::msg::ActionStatus::Cancelled : pnm::msg::ActionStatus::Succeeded;
            if (!result || !*result || (**result).status != expected || (**result).value != (cancel ? 25U : 100U))
                throw std::runtime_error("Unexpected terminal action result");
            std::cout << "UI: " << (cancel ? "cancelled" : "succeeded") << ", count " << (**result).value << '\n';
        }
        io_bridge.stop(); ui_bridge.stop();
        if (io_bridge.failure() || ui_bridge.failure()) throw std::runtime_error("Bridge failed");
        std::cout << "Action completion and cancellation verified.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "p2p action example: " << error.what() << '\n';
        return 1;
    }
}
