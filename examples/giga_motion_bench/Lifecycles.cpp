#include "Link.hpp"

namespace motion_bench
{
    void topics(const Request& r, Report& report, std::shared_ptr<hal::IInterCoreChannel> channel)
    {
        auto& endpoint = link(channel);
        endpoint.start();
        auto topic = endpoint.bus.topic<^^Topics::data>();
        if constexpr (SIDE == 0) {
            std::uint32_t last{};
            bool first = true;
            auto subscription = topic.subscribe([&](const std::string& value) {
                auto frame = std::as_bytes(std::span(value));
                auto time = now();
                if (!valid(frame) || frame.size() != r.bytes) {
                    ++report.errors;
                    return;
                }
                auto sequence = read32(frame, 0);
                if (!first && sequence <= last)
                    ++report.errors;
                first = false;
                last = sequence;
                ++report.received;
                report.latency.add(time - read32(frame, 4));
                report.processing.add(time - read32(frame, 8));
            }, pnm::msg::SubscriptionOptions::latestOnly());
            release(shared.armed, acquire(shared.sequence));
            const auto end = std::chrono::steady_clock::now() +
                             std::chrono::microseconds{ std::uint64_t(r.count) * 1000000 /
                                                        std::max<std::uint32_t>(r.rate, 1) } +
                             5s;
            while ((first || last + 1 != r.count) && std::chrono::steady_clock::now() < end)
                subscription.poll(10ms);
            if (first || last + 1 != r.count)
                ++report.errors;
            report.rejected =
              r.count - report.received; // Explicitly coalesced, not corrupt/lost trajectories.
        }
        else {
            while (acquire(shared.armed) != acquire(shared.sequence))
                std::this_thread::sleep_for(1ms);
            std::array<std::byte, 2000> buffer{};
            for (unsigned i = 0; i < r.count; ++i) {
                const auto scheduled = r.start + (r.rate ? i * (1000000 / r.rate) : 0);
                while (!reached(scheduled))
                    std::this_thread::sleep_for(1ms);
                const auto time = now();
                packet(std::span(buffer).first(r.bytes), i, scheduled, time);
                topic.publish(std::string(reinterpret_cast<const char*>(buffer.data()), r.bytes));
                report.scheduling.add(now() - scheduled);
                ++report.sent;
            }
        }
        report.elapsed = now() - r.start;
    }
    void actions(const Request& r, Report& report, std::shared_ptr<hal::IInterCoreChannel> channel)
    {
        auto& endpoint = link(channel);
        endpoint.start();
        auto action = endpoint.bus.action<^^Actions::action>();
        runtime::Notification changed;
        if constexpr (SIDE == 0) {
            using Execution = pnm::msg::ActionExecution<std::uint32_t, std::uint32_t>;
            std::optional<Execution> execution;
            std::uint32_t submitted{}, accepted{};
            auto provider = action.serveDeferred([&](std::uint32_t value, Execution next) {
                if (execution) {
                    next.fail(pnm::msg::ActionError::Busy);
                    ++report.errors;
                    return;
                }
                submitted = value;
                accepted = now();
                report.latency.add(accepted - submitted);
                if (!next.accept()) {
                    ++report.errors;
                    return;
                }
                next.feedback(value ^ 0x5a5a);
                execution.emplace(std::move(next));
            });
            auto watch = pnm::msg::watch(provider, [&] { changed.signal(); });
            release(shared.armed, acquire(shared.sequence));
            const auto end =
              std::chrono::steady_clock::now() + std::chrono::milliseconds{ r.count * 500 } + 5s;
            while (report.received < r.count && std::chrono::steady_clock::now() < end) {
                provider.poll();
                if (execution) {
                    if (execution->cancelRequested()) {
                        execution->cancelled(submitted + 1);
                        execution.reset();
                        ++report.received;
                    }
                    else if (!r.fixture && now() - accepted >= 20000) {
                        execution->succeed(submitted + 1);
                        execution.reset();
                        ++report.received;
                    }
                }
                changed.waitUntil(std::chrono::steady_clock::now() + 1ms);
            }
            if (execution) {
                execution->fail(pnm::msg::ActionError::Unavailable);
                execution.reset();
            }
            if (report.received != r.count)
                ++report.errors;
        }
        else {
            while (acquire(shared.armed) != acquire(shared.sequence))
                std::this_thread::sleep_for(1ms);
            for (unsigned i = 0; i < r.count; ++i) {
                const auto submitted = now();
                bool accepted{}, feedback{}, cancel_sent{};
                std::optional<pnm::msg::ActionResult<std::uint32_t>> result;
                auto goal = action.sendGoal(submitted, { .accept_timeout = 500ms }, { .on_accepted = [&] {
                    accepted = true;
                    report.latency.add(now() - submitted);
                }, .on_feedback = [&](const std::uint32_t& value) {
                    feedback = value == (submitted ^ 0x5a5a);
                }, .on_result = [&](auto value) { result.emplace(std::move(value)); } });
                auto watch = pnm::msg::watch(goal, [&] { changed.signal(); });
                const auto end = std::chrono::steady_clock::now() + 2s;
                while (!goal.poll() && std::chrono::steady_clock::now() < end) {
                    if (accepted && r.fixture && !cancel_sent) {
                        goal.requestCancel();
                        cancel_sent = true;
                    }
                    changed.waitUntil(std::chrono::steady_clock::now() + 1ms);
                }
                if (!result) {
                    goal.requestCancel();
                    ++report.errors;
                    break;
                }
                const auto expected =
                  r.fixture ? pnm::msg::ActionStatus::Cancelled : pnm::msg::ActionStatus::Succeeded;
                if (!accepted || !feedback || !*result || (**result).status != expected ||
                    (**result).value != submitted + 1)
                    ++report.errors;
                report.processing.add(now() - submitted);
                ++report.sent;
            }
        }
        report.elapsed = now() - r.start;
    }
}

namespace motion_bench
{
    void flood(const Request& r, Report& report, std::shared_ptr<hal::IInterCoreChannel> channel)
    {
        auto& endpoint = link(channel);
        endpoint.start();
        runtime::Notification changed;
        if constexpr (SIDE == 0) {
            auto urgent = endpoint.bus.service<^^Services::urgent>().serve([&](const std::string& value) {
                const auto frame = std::as_bytes(std::span(value));
                if (!valid(frame))
                    ++report.errors;
                report.latency.add(now() - read32(frame, 4));
                ++report.received;
                return value;
            });
            auto normal = endpoint.bus.topic<^^Topics::data>().subscribe(
              [&](const std::string&) { ++report.value; }, pnm::msg::SubscriptionOptions::latestOnly());
            auto background = endpoint.bus.topic<^^Topics::log>().subscribe(
              [&](const std::string&) { ++report.maximum_backlog; }, { .capacity = 8 });
            auto watch = pnm::msg::watch(urgent, [&] { changed.signal(); });
            release(shared.armed, acquire(shared.sequence));
            const auto end =
              std::chrono::steady_clock::now() + std::chrono::milliseconds{ r.count * 500 } + 5s;
            while (acquire(shared.uiDone) != acquire(shared.sequence) &&
                   std::chrono::steady_clock::now() < end) {
                urgent.poll();
                normal.poll();
                background.poll();
                changed.waitUntil(std::chrono::steady_clock::now() + 1ms);
            }
        }
        else {
            while (acquire(shared.armed) != acquire(shared.sequence))
                std::this_thread::sleep_for(1ms);
            std::uint32_t published{};
            auto producer = runtime::thread::create_jthread(
              { .name = "bench.flood", .priority = dispatchMode ? 12 : 1, .stack_size = 8192 }, [&](std::stop_token stop) {
                auto normal = endpoint.bus.topic<^^Topics::data>();
                auto background = endpoint.bus.topic<^^Topics::log>();
                const auto deadline = now() + 2'000'000U;
                // An overloaded lower-priority command thread cannot stop us.
                // Bound this deliberate priority-inversion stress at its source.
                while (!stop.stop_requested() && !reached(deadline)) {
                    for (unsigned burst = 0; burst < 16; ++burst) {
                        normal.publish(std::string(1536, 'n'));
                        background.publish(std::string(480, 'b'));
                        ++published;
                    }
                    std::this_thread::sleep_for(5ms);
                }
            });
            auto urgent = endpoint.bus.service<^^Services::urgent>();
            std::array<std::byte, 32> bytes{};
            for (unsigned i = 0; i < r.count; ++i) {
                const auto scheduled = r.start + i * 10000;
                while (!reached(scheduled))
                    std::this_thread::sleep_for(1ms);
                auto before = now();
                packet(bytes, i, scheduled, before);
                std::string value(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                const auto response = urgent.request(value, 1s).get();
                if (!response || *response != value)
                    ++report.errors;
                report.latency.add(now() - scheduled);
                report.processing.add(now() - before);
                ++report.sent;
            }
            producer.request_stop();
            producer.join();
            report.value = published;
            if (auto stats = endpoint.bridge.topicStatistics("log"))
                report.rejected = stats->dropped;
        }
        report.elapsed = now() - r.start;
    }
}
