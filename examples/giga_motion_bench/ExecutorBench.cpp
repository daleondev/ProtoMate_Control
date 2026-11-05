#include "Bench.hpp"
#include "Link.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/util/StepHardware.hpp"
#include "hal/stm32/InterruptGuard.hpp"
#include "io/motion/Executor.hpp"
#include "io/motion/ProfileMemory.hpp"

extern "C" volatile std::uint32_t giga_step_irq_cycles_max;
extern "C" std::uint32_t giga_motion_bench_buffer_edges;
extern "C" volatile std::uint32_t giga_motion_bench_worst_refill_cycles;
extern "C" volatile std::uint32_t giga_motion_bench_worst_budget_ticks;

namespace motion_bench
{
    void executorBench(const Request& r, Report& report, std::shared_ptr<hal::IInterCoreChannel> channel)
    {
        // Match the application: executor priority 1, M4 ICC priority 4.
        // The earlier transport experiments deliberately lowered ICC to 5;
        // under pulse preparation + reserved work that can starve heartbeats.
        protectedAdmission = false;
        auto& ordinary = link(channel);
        if (ordinary.protectedScheduling)
            throw std::runtime_error("reset before executor: production ICC priorities required");
        ordinary.start();
        if constexpr (SIDE == 0) {
            UINT oldPriority{};
            tx_thread_priority_change(tx_thread_identify(), 1, &oldPriority);
            static auto generator = hal::core::create_stepper_generator();

            static std::array<std::shared_ptr<hal::IDigitalOutput>, 3> directions;
            static std::array<std::shared_ptr<hal::IStepOutput>, 3> axes;

            static io::motion::ProfileMemory profileMemory;
            static std::unique_ptr<io::motion::Executor> executor;
            static std::uint64_t commandId{};
            if (!executor)
                executor = std::make_unique<io::motion::Executor>(
                  generator,
                  [&](unsigned axis, bool forward, std::shared_ptr<const hal::step::Sequence> sequence) {
                    directions[axis]->write(forward ? hal::gpio::Level::High : hal::gpio::Level::Low);
                    return bool(axes[axis]->prepareSequence(std::move(sequence)));
                }, &profileMemory);
            io::motion::Executor::Command command;
            for (unsigned axis = 0; axis < 3; ++axis) {
                if (!axes[axis])
                    axes[axis] = generator->output(hal::step::Axis(axis));
                if (!directions[axis])
                    directions[axis] = hal::core::create_stepper_direction_output(hal::core::MotorId(axis));
                constexpr double rate = io::motion::Executor::MAX_PRODUCTION_RATE;
                command.axes[axis].limits = { rate, rate * 10, rate * 10, rate * 100 };
                const auto ratio = r.lane == 1 ? std::array{ 1., .8, .5 }[axis] : 1.;
                auto& limits = command.axes[axis].limits;
                limits.velocity *= ratio;
                limits.acceleration *= ratio;
                limits.deceleration *= ratio;
                limits.jerk *= ratio;
                const auto count = r.lane == 2 ? std::array{ .4, .2, .048 }[axis] * rate : .4 * rate * ratio;
                command.axes[axis].profile = motion::Profile::create(count, limits)->description();
            }
            const auto nominal = motion::Profile::fromDescription(*command.axes[0].profile);
            const auto firstTick = std::uint32_t(std::ceil(nominal->timeAt(1) * 1e7));
            std::atomic_uint loadPeriods{};
            std::jthread load;
            // Motion (1), reserved work (2), notifications (3), dispatch (4).
            // Real-time work cannot be starved by an ordinary traffic backlog;
            // notification delivery cannot be starved by its own dispatcher.
            if (r.fixture)
                load = runtime::thread::create_jthread(
                  { .name = "bench.reserve", .priority = 2, .stack_size = 4096 }, [&](std::stop_token stop) {
                    static auto pin = hal::gpio::create_output({ .pin = { hal::gpio::Port::B, 11 } });
                    const auto burn = [](unsigned n) {
                        asm volatile("1: subs %0, %0, #1\n bne 1b" : "+r"(n)::"cc");
                    };
                    unsigned calibration;
                    {
                        const hal::stm32::InterruptGuard guard;
                        const auto start = cycles();
                        burn(1000);
                        calibration = cycles() - start;
                    }
                    const auto work = std::max<std::uint32_t>(1U, r.fixture * 2'400'000U / calibration);
                    auto next = std::chrono::steady_clock::now();
                    while (!stop.stop_requested()) {
                        next += 1ms;
                        pin->write(hal::gpio::Level::High);
                        burn(work);
                        pin->write(hal::gpio::Level::Low);
                        ++loadPeriods;
                        std::this_thread::sleep_until(next);
                    }
                });
            // Same 32-pulse halves as the production executor. Future edges
            // remain replaceable through the guarded time-based splice.
            giga_motion_bench_buffer_edges = 2 * io::motion::Executor::DMA_HALF_PULSES;
            giga_step_irq_cycles_max = 0;
            giga_motion_bench_worst_refill_cycles = 0;
            giga_motion_bench_worst_budget_ticks = 0;
            release(shared.armed, acquire(shared.sequence));
            const auto started = now();
            for (unsigned repetition = 0; repetition < r.count; ++repetition) {
                if (motorEnable->read() != hal::gpio::Level::High)
                    hal::panic("profile enable");
                command.id = ++commandId;
                const auto prepare = now();
                if (!generator->start())
                    hal::panic("executor timebase");
                if (!executor->prepare(command)) {
                    ++report.errors;
                    break;
                }
                report.latency.add(now() - prepare);
                const auto admission = now();
                if (!executor->start(command.id)) {
                    ++report.errors;
                    break;
                }
                report.processing.add(now() - admission);
                const auto deadline = std::chrono::steady_clock::now() + 2s;
                bool injected{};
                while (executor->active() && std::chrono::steady_clock::now() < deadline) {
                    const auto tick = now();
                    const auto before = executor->status();
                    if (r.direction && !injected && before.elapsed >= r.bytes * 10'000) {
                        injected = true;
                        const auto time = generator->timeline();
                        if (time)
                            report.executor_request = hal::util::step_distance(
                              hal::util::step_add(time->first_rise[0], hal::util::STEP_PARK - firstTick),
                              time->tick);
                        if (r.direction == 1)
                            executor->stop(command.id);
                        if (r.direction == 3)
                            std::this_thread::sleep_for(700ms);
                    }
                    if (!injected)
                        executor->renew(command.id);
                    executor->service();
                    report.scheduling.add(now() - tick);
                    std::this_thread::sleep_for(1ms);
                }
                const auto status = executor->status();
                report.executor_build_us = std::max(report.executor_build_us, status.maximum_build_us);
                report.executor_splice_us = std::max(report.executor_splice_us, status.maximum_splice_us);
                report.executor_error = status.last_error;
                report.executor_cut = status.stopping_at;

                report.executor_reason = unsigned(status.reason);
                report.rejected += status.rejected_renewals;
                if (executor->active() || status.state == io::motion::Executor::State::Faulted ||
                    (!r.direction && status.state != io::motion::Executor::State::Completed) ||
                    (r.direction && status.state != io::motion::Executor::State::Stopped))
                    ++report.errors;
                for (unsigned axis = 0; axis < 3; ++axis)
                    if (!status.counts_exact ||
                        (!r.direction &&
                         status.pulses[axis] != std::uint64_t(command.axes[axis].profile->distance)))
                        ++report.errors;
                report.received += status.pulses[0];
                report.value += status.pulses[1];
                report.checksum += status.pulses[2];
                if (executor->active())
                    executor->abort();
                ++report.sent;
                if (report.errors)
                    break;
                std::this_thread::sleep_for(10ms);
            }
            report.elapsed = now() - started;
            if (load.joinable()) {
                load.request_stop();
                load.join();
            }
            report.clock = loadPeriods;
            report.maximum_backlog = giga_step_irq_cycles_max;
            // These two profile-specific fields form a pair: 240 MHz core
            // cycles versus 10 MHz ticks left until the armed guard deadline.
            report.refill_cycles = giga_motion_bench_worst_refill_cycles;
            report.refill_budget_ticks = giga_motion_bench_worst_budget_ticks;
            static_cast<void>(generator->stop());
            for (auto& axis : axes)
                static_cast<void>(axis->clear());
            giga_motion_bench_buffer_edges = 512;
            if (motorEnable->read() != hal::gpio::Level::High)
                ++report.errors;
            tx_thread_priority_change(tx_thread_identify(), oldPriority, &oldPriority);
        }
        else {
            while (acquire(shared.armed) != acquire(shared.sequence))
                std::this_thread::sleep_for(1ms);
            auto state = ordinary.bus.topic<^^Topics::data>();
            auto log = ordinary.bus.topic<^^Topics::log>();
            const auto start = now();
            auto nextLog = start;
            while (acquire(shared.done) != acquire(shared.sequence)) {
                state.publish(std::string(r.traffic ? 512 : 256, 's'));
                if (r.traffic || reached(nextLog)) {
                    log.publish(std::string(r.traffic ? 480 : 128, 'b'));
                    nextLog = now() + 100'000;
                }
                ++report.sent;
                if (now() - start > r.count * 700000U + 5'000'000U) {
                    ++report.errors;
                    break;
                }
                std::this_thread::sleep_for(r.traffic ? 10ms : 20ms);
            }
            report.elapsed = now() - start;
        }
        const auto connection = ordinary.bridge.connectionStatus();
        report.icc_state = unsigned(connection.state);
        report.icc_reason = unsigned(connection.reason);
        if (connection.state != icc::ConnectionState::Connected)
            ++report.errors;
    }
}
