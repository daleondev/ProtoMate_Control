#include "Link.hpp"
#include "common/icc/transport/InterCoreTransport.hpp"
#include "hal/util/InterCoreTrace.hpp"
#include <algorithm>
#include <condition_variable>
#include <mutex>

namespace motion_bench
{
    void raw(const Request& r, Report& report, hal::IInterCoreChannel& channel)
    {
        const auto lane = static_cast<hal::intercore::Lane>(r.lane);
        const auto send_side = r.direction == 2 || r.direction == SIDE;
        const auto receive_side = r.direction == 2 || r.direction != SIDE;
        std::array<std::byte, 2048> bytes{};
        const auto period = r.rate ? 1'000'000 / r.rate : 0;
        const auto deadline = r.start + std::max<std::uint32_t>(2'000'000U, r.count * period + 1'000'000U);
        std::uint32_t sequence{}, expected{};
        runtime::Notification notification;
        channel.setNotification([&]() noexcept { notification.signal(); });

        while (!reached(deadline)) {
            bool progress{};
            if (send_side && sequence < r.count && reached(r.start + sequence * period)) {
                const auto scheduled = r.start + sequence * period;
                const auto time = now();
                packet(std::span(bytes).first(r.bytes), sequence, scheduled, time);
                const auto result = channel.trySend(std::span(bytes).first(r.bytes), lane);
                if (result) {
                    report.scheduling.add(time - scheduled);
                    ++report.sent;
                    ++sequence;
                    progress = true;
                }
                else if (result.error() == hal::IInterCoreChannel::Error::WouldBlock) {
                    ++report.rejected;
                }
                else {
                    ++report.errors;
                    break;
                }
            }
            if (receive_side && report.received < r.count) {
                const auto result = channel.tryReceive(bytes, lane);
                if (result) {
                    const auto received = now();
                    const auto frame = std::span(bytes).first(*result);
                    if (*result != r.bytes || !valid(frame) || read32(frame, 0) != expected++)
                        ++report.errors;
                    report.latency.add(received - read32(frame, 4));
                    report.processing.add(received - read32(frame, 8));
                    ++report.received;
                    progress = true;
                }
                else if (result.error() != hal::IInterCoreChannel::Error::WouldBlock) {
                    ++report.errors;
                    break;
                }
            }
            if ((!send_side || sequence == r.count) && (!receive_side || report.received == r.count))
                break;
            // Busy pacing is intentional only in the raw bandwidth benchmark.
            // Notification-driven idle receiving is measured separately by transport.
            if (!progress && !send_side)
                notification.waitUntil(std::chrono::steady_clock::now() + 1ms);
        }
        channel.setNotification({});
        report.elapsed = now() - r.start;
        if ((send_side && report.sent != r.count) || (receive_side && report.received != r.count))
            ++report.errors;
    }
    Link& link(std::shared_ptr<hal::IInterCoreChannel> channel)
    {
        static Link value{ std::move(channel) };
        return value;
    }
    struct Yield
    {
        pnm::coro::Context* context;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> handle) const { context->schedule(handle); }
        void await_resume() const noexcept {}
    };
    pnm::coro::Task<void> transfer(Link& endpoint,
                                   const Request& r,
                                   Report& report,
                                   bool sending,
                                   std::stop_token stop)
    {
        std::array<std::byte, 2048> bytes{};
        const auto lane = static_cast<icc::Lane>(r.lane);
        const auto period = r.rate ? 1'000'000 / r.rate : 0;
        for (unsigned sequence = 0; sequence < r.count; ++sequence) {
            if (sending) {
                const auto scheduled = r.start + sequence * period;
                while (!reached(scheduled) && !stop.stop_requested())
                    co_await pnm::coro::sleep(endpoint.context, 100us, stop);
                const auto time = now();
                packet(std::span(bytes).first(r.bytes), sequence, scheduled, time);
                auto result = co_await endpoint.transport.send(std::span(bytes).first(r.bytes), lane, stop);
                if (!result) {
                    ++report.errors;
                    co_return;
                }
                report.scheduling.add(now() - scheduled);
                ++report.sent;
            }
            else {
                auto result = co_await endpoint.transport.receive(bytes, lane, stop);
                if (!result) {
                    ++report.errors;
                    co_return;
                }
                auto time = now();
                auto frame = std::span(bytes).first(*result);
                if (*result != r.bytes || !valid(frame) || read32(frame, 0) != sequence)
                    ++report.errors;
                report.latency.add(time - read32(frame, 4));
                report.processing.add(time - read32(frame, 8));
                ++report.received;
            }
            co_await Yield{ &endpoint.context };
        }
    }
    struct Completion
    {
        std::mutex mutex;
        std::condition_variable changed;
        unsigned pending{};
    };
    pnm::coro::Task<void> trackedTransfer(Link& endpoint,
                                          const Request& r,
                                          Report& report,
                                          bool sending,
                                          std::stop_token stop,
                                          std::shared_ptr<Completion> completion)
    {
        try {
            co_await transfer(endpoint, r, report, sending, stop);
        } catch (...) {
            ++report.errors;
        }
        // The coroutine retains the completion object until after notification.
        // The caller observes reports only after all tasks release this lock.
        std::lock_guard lock(completion->mutex);
        --completion->pending;
        completion->changed.notify_all();
    }
    void transport(const Request& r, Report& report, std::shared_ptr<hal::IInterCoreChannel> channel)
    {
        auto& endpoint = link(channel);
        auto completion = std::make_shared<Completion>();
        completion->pending = r.direction == 2 ? 2 : 1;
        std::stop_source stop;
        if (r.direction == 2 || r.direction == SIDE)
            pnm::coro::co_spawn(endpoint.context,
                                trackedTransfer(endpoint, r, report, true, stop.get_token(), completion));
        if (r.direction == 2 || r.direction != SIDE)
            pnm::coro::co_spawn(endpoint.context,
                                trackedTransfer(endpoint, r, report, false, stop.get_token(), completion));
        const auto deadline =
          std::chrono::steady_clock::now() +
          std::chrono::microseconds{ r.rate ? std::uint64_t(r.count) * 1'000'000 / r.rate : 0 } + 5s;
        std::unique_lock lock(completion->mutex);
        if (!completion->changed.wait_until(lock, deadline, [&] { return completion->pending == 0; })) {
            lock.unlock();
            stop.request_stop();
            lock.lock();
            if (!completion->changed.wait_for(lock, 2s, [&] { return completion->pending == 0; }))
                hal::panic("benchmark transport cancellation did not drain");
        }
        report.elapsed = now() - r.start;
    }
    void bridge(const Request& r, Report& report, std::shared_ptr<hal::IInterCoreChannel> channel)
    {
        auto& endpoint = link(channel);
        endpoint.start();
        runtime::Notification changed;
        if constexpr (SIDE == 0) {
            auto handler = [&](const std::string& value) {
                const auto time = now();
                const auto frame = std::as_bytes(std::span(value));
                hal::util::intercoreTrace(hal::util::InterCoreStage::Handler, unsigned(r.lane), frame);
                if (!valid(frame) || frame.size() != r.bytes)
                    ++report.errors;
                report.latency.add(time - read32(frame, 4));
                report.processing.add(time - read32(frame, 8));
                ++report.received;
                return value;
            };
            auto normal = endpoint.bus.service<^^Services::echo>().serve(handler);
            auto urgent = endpoint.bus.service<^^Services::urgent>().serve(handler);
            auto n = pnm::msg::watch(normal, [&] { changed.signal(); });
            auto u = pnm::msg::watch(urgent, [&] { changed.signal(); });
            release(shared.armed, acquire(shared.sequence));
            const auto deadline =
              std::chrono::steady_clock::now() +
              std::chrono::microseconds{ r.rate ? std::uint64_t(r.count) * 1'000'000 / r.rate : 0 } + 60s;
            while (report.received < r.count && std::chrono::steady_clock::now() < deadline) {
                urgent.poll();
                normal.poll();
                if (report.received < r.count)
                    changed.waitUntil(deadline);
            }
            if (report.received != r.count)
                ++report.errors;
        }
        else {
            while (acquire(shared.armed) != acquire(shared.sequence))
                std::this_thread::sleep_for(1ms);
            auto service = r.lane == 1 ? endpoint.bus.service<^^Services::urgent>()
                                       : endpoint.bus.service<^^Services::echo>();
            std::array<std::byte, 2000> bytes{};
            const auto period = r.rate ? 1'000'000 / r.rate : 0;
            for (unsigned i = 0; i < r.count; ++i) {
                const auto scheduled = r.start + i * period;
                while (!reached(scheduled))
                    std::this_thread::sleep_for(1ms);
                const auto time = now();
                packet(std::span(bytes).first(r.bytes), i, scheduled, time);
                std::string value{ reinterpret_cast<const char*>(bytes.data()), r.bytes };
                hal::util::intercoreTrace(
                  hal::util::InterCoreStage::Submission, unsigned(r.lane), std::span(bytes).first(r.bytes));
                auto call = service.request(value, 1s);
                hal::util::intercoreTrace(
                  hal::util::InterCoreStage::Enqueued, unsigned(r.lane), std::span(bytes).first(r.bytes));
                auto response = call.get();
                hal::util::intercoreTrace(
                  hal::util::InterCoreStage::Completed, unsigned(r.lane), std::span(bytes).first(r.bytes));
                if (!response || *response != value)
                    ++report.errors;
                report.latency.add(now() - scheduled);
                report.processing.add(now() - time);
                report.scheduling.add(time - scheduled);
                ++report.sent;
            }
        }
        report.elapsed = now() - r.start;
    }
}
