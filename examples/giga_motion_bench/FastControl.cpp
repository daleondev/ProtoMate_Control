#include "Link.hpp"
#include "hal/stm32/InterruptGuard.hpp"
#include "hal/util/InterCoreMemory.hpp"
#include "hal/drivers/factory/gpio.hpp"

namespace
{
    using namespace motion_bench;
    // Dedicated experiment, independent of the live ICC queues. MPU maps this
    // reserved SDRAM as Normal, shared, non-cacheable on both cores.
    __attribute__((section(".motion_bench_fast"), aligned(32))) hal::util::InterCoreMemory memory;
    runtime::Notification* notification{};
    constexpr unsigned LOCAL = SIDE + 6;
    constexpr unsigned PEER = (1 - SIDE) + 6;
    void notifyPeer()
    {
        const hal::stm32::InterruptGuard guard;
        __DMB();
        if (HAL_HSEM_FastTake(PEER) != HAL_OK)
            hal::panic("bench fast doorbell");
        HAL_HSEM_Release(PEER, 0);
    }
    struct Endpoint
    {
        runtime::Notification changed;
        Endpoint()
        {
            if constexpr (SIDE == 1) {
                memory = {};
                release(shared.fastReady, acquire(shared.sequence));
            }
            else {
                while (acquire(shared.fastReady) != acquire(shared.sequence))
                    std::this_thread::sleep_for(1ms);
            }
            const hal::stm32::InterruptGuard guard;
            notification = &changed;
            __HAL_HSEM_CLEAR_FLAG(1U << LOCAL);
            HAL_HSEM_ActivateNotification(1U << LOCAL);
        }
        ~Endpoint()
        {
            const hal::stm32::InterruptGuard guard;
            HAL_HSEM_DeactivateNotification(1U << LOCAL);
            notification = nullptr;
        }
        auto send(std::span<const std::byte> bytes)
        {
            auto result = memory.push(1 - SIDE, bytes, hal::intercore::Lane::Urgent);
            if (result) notifyPeer();
            return result;
        }
        auto receive(std::span<std::byte> bytes)
        {
            auto result = memory.pop(SIDE, bytes, hal::intercore::Lane::Urgent);
            if (result) notifyPeer();
            return result;
        }
    };
}

// Called by the existing HAL HSEM IRQ owner; no second interrupt handler.
extern "C" void motion_bench_doorbell(std::uint32_t mask) noexcept
{
    if ((mask & (1U << LOCAL)) && notification) {
        HAL_HSEM_ActivateNotification(1U << LOCAL);
        notification->signal();
    }
}

namespace motion_bench
{
    void fastControl(const Request& r, Report& report, std::shared_ptr<hal::IInterCoreChannel> channel)
    {
        UINT oldPriority{};
        tx_thread_priority_change(tx_thread_identify(), SIDE == 0 ? 1 : 2, &oldPriority);
        protectedAdmission = true;
        auto& ordinary = link(channel);
        if (!ordinary.protectedScheduling) hal::panic("fast requires fresh link; reset first");
        ordinary.start();
        Endpoint fast;
        std::atomic_uint normalCount{}, backgroundCount{};
        std::jthread background;
        if (r.fixture) {
            background = runtime::thread::create_jthread(
              { .name = "bench.bulk", .priority = 12, .stack_size = 8192 }, [&](std::stop_token stop) {
                  if constexpr (SIDE == 0) {
                      auto n = ordinary.bus.topic<^^Topics::data>().subscribe([&](const auto&) { ++normalCount; }, pnm::msg::SubscriptionOptions::latestOnly());
                      auto b = ordinary.bus.topic<^^Topics::log>().subscribe([&](const auto&) { ++backgroundCount; }, { .capacity = 8 });
                      while (!stop.stop_requested()) {
                          n.poll(); b.poll();
                          std::this_thread::sleep_for(1ms);
                      }
                  }
                  else {
                      const std::string normal(1536, 'n'), log(480, 'b');
                      auto n = ordinary.bus.topic<^^Topics::data>();
                      auto b = ordinary.bus.topic<^^Topics::log>();
                      while (!stop.stop_requested()) {
                          for (unsigned i = 0; i < 16; ++i) {
                              n.publish(normal); b.publish(log);
                              ++normalCount;
                          }
                          std::this_thread::sleep_for(1ms);
                      }
                  }
              });
        }
        static std::shared_ptr<hal::IStepGenerator> generator;
        std::jthread reserve;
        runtime::Notification steps;
        std::uint32_t expectedPulses{};
        std::atomic_uint reservePeriods{};
        if constexpr (SIDE == 0) {
            if (r.fixture >= 2) {
                if (motorEnable->read() != hal::gpio::Level::High) hal::panic("fast enable active");
                if (!generator) generator = hal::core::create_stepper_generator();
                expectedPulses = (std::uint64_t(r.count) * 100000 / r.rate) + 100000;
                for (unsigned axis = 0; axis < 3; ++axis) {
                    auto out = generator->output(static_cast<hal::step::Axis>(axis));
                    if (!out->prepare({10us, 5us}, expectedPulses)) hal::panic("fast steps prepare");
                }
                if (!generator->setProgressCallback([&](const auto& status) noexcept {
                        if (status.state == hal::step::State::Underrun || status.state == hal::step::State::DmaError ||
                            std::ranges::all_of(status.axes, [](auto s) { return s == hal::step::State::Completed; }))
                            steps.signal();
                    }) ||
                    !generator->start() || !generator->startPrepared({1ms, 1ms, 1ms}))
                    hal::panic("fast steps start");
                reserve = runtime::thread::create_jthread(
                  {.name="bench.reserve", .priority=3, .stack_size=4096}, [&](std::stop_token stop) {
                    static auto marker = hal::gpio::create_output({.pin={hal::gpio::Port::B, 11}});
                    const auto burn = [](unsigned count) {
                        asm volatile("1: subs %0, %0, #1\n bne 1b" : "+r"(count) :: "cc");
                    };
                    unsigned calibration{};
                    {
                        const hal::stm32::InterruptGuard guard;
                        const auto begin = cycles();
                        burn(1000);
                        calibration = cycles() - begin;
                    }
                    const auto work = std::max(1U, 48'000'000U / calibration);
                    auto next = std::chrono::steady_clock::now();
                    while (!stop.stop_requested()) {
                        next += 1ms;
                        marker->write(hal::gpio::Level::High);
                        burn(work); // Fixed work: preemption does not consume the reservation.
                        marker->write(hal::gpio::Level::Low);
                        ++reservePeriods;
                        std::this_thread::sleep_until(next);
                    }
                  });
            }
        }
        std::array<std::byte, 32> frame{};
        const auto timeout = std::chrono::steady_clock::now() +
                             std::chrono::microseconds{std::uint64_t(r.count) * 1'000'000 / r.rate} + 10s;
        if constexpr (SIDE == 0) {
            release(shared.armed, acquire(shared.sequence));
            while (report.received < r.count && std::chrono::steady_clock::now() < timeout) {
                if (const auto received = fast.receive(frame)) {
                    const auto time = now();
                    if (*received != frame.size() || !valid(frame) || read32(frame, 0) != report.received)
                        ++report.errors;
                    report.latency.add(time - read32(frame, 4));
                    report.processing.add(time - read32(frame, 8));
                    ++report.received;
                    while (!fast.send(frame)) {
                        ++report.rejected;
                        if (std::chrono::steady_clock::now() >= timeout) break;
                        fast.changed.waitUntil(timeout);
                    }
                }
                else fast.changed.waitUntil(timeout);
            }
            if (report.received != r.count) ++report.errors;
        }
        else {
            while (acquire(shared.armed) != acquire(shared.sequence))
                std::this_thread::sleep_for(1ms);
            // Start after both worker/bridge setups; include every subsequent
            // delayed submission against this fixed open-loop schedule.
            const auto start = now();
            for (unsigned i = 0; i < r.count; ++i) {
                const auto scheduled = start + i * (1'000'000 / r.rate);
                while (!reached(scheduled)) std::this_thread::sleep_for(1ms);
                const auto sent = now();
                packet(frame, i, scheduled, sent);
                while (!fast.send(frame)) {
                    ++report.rejected;
                    if (std::chrono::steady_clock::now() >= timeout) break;
                    fast.changed.waitUntil(timeout);
                }
                auto result = fast.receive(frame);
                while (!result && std::chrono::steady_clock::now() < timeout) {
                    fast.changed.waitUntil(timeout);
                    result = fast.receive(frame);
                }
                if (!result || *result != frame.size() || !valid(frame) || read32(frame, 0) != i) {
                    ++report.errors;
                    break;
                }
                report.latency.add(now() - scheduled);
                report.processing.add(now() - sent);
                report.scheduling.add(sent - scheduled);
                ++report.sent;
            }
        }
        if constexpr (SIDE == 0) {
            if (r.fixture >= 2) {
                const auto end = std::chrono::steady_clock::now() + 3s;
                while (std::chrono::steady_clock::now() < end) {
                    const auto status = generator->status();
                    if (std::ranges::all_of(status.axes, [](auto s) { return s == hal::step::State::Completed; })) break;
                    if (status.state == hal::step::State::Underrun || status.state == hal::step::State::DmaError) break;
                    steps.waitUntil(end);
                }
                const auto status = generator->stop();
                static_cast<void>(generator->setProgressCallback({}));
                if (!status.counts_exact || std::ranges::any_of(status.pulses, [&](auto n) { return n != expectedPulses; }) ||
                    motorEnable->read() != hal::gpio::Level::High) ++report.errors;
                report.checksum = status.pulses[0];
                reserve.request_stop(); reserve.join();
                report.clock = reservePeriods;
            }
        }
        if (background.joinable()) {
            background.request_stop();
            background.join();
        }
        report.value = normalCount;
        report.maximum_backlog = backgroundCount;
        report.elapsed = now() - r.start;
        tx_thread_priority_change(tx_thread_identify(), oldPriority, &oldPriority);
    }
}
