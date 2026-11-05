#include "Bench.hpp"
#include "FixedSequence.hpp"
#include "Link.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/stm32/InterruptGuard.hpp"

extern "C" volatile std::uint32_t giga_step_irq_cycles_max;
extern "C" std::uint32_t giga_motion_bench_buffer_edges;
extern "C" volatile std::uint32_t giga_motion_bench_worst_refill_cycles;
extern "C" volatile std::uint32_t giga_motion_bench_worst_budget_ticks;

namespace motion_bench
{
    void profileBench(const Request& r, Report& report, std::shared_ptr<hal::IInterCoreChannel> channel)
    {
        protectedAdmission = true;
        auto& ordinary = link(channel);
        ordinary.start();
        if constexpr (SIDE == 0) {
            UINT oldPriority{};
            tx_thread_priority_change(tx_thread_identify(), 1, &oldPriority);
            static auto generator = hal::core::create_stepper_generator();
            static std::array<std::optional<FixedSequence>,3> storage;
            static std::array<std::shared_ptr<hal::IDigitalOutput>,3> directions;
            static std::array<std::shared_ptr<hal::IStepOutput>,3> axes;
            std::array<std::shared_ptr<FixedSequence>,3> sequences;
            const std::array<double,3> scales = r.direction ? std::array{1.,1.,1.} : std::array{1.,.8,.5};
            std::array<std::optional<std::chrono::nanoseconds>,3> delays;
            runtime::Notification complete;
            const auto setup = now();
            for (unsigned axis=0; axis<3; ++axis) {
                const double speed = r.rate * scales[axis];
                const auto profile = motion::Profile::create(speed * .4, {speed, speed * 10, speed * 10, speed * 100});
                if (!profile) hal::panic("profile fixture");
                storage[axis].emplace(*profile);
                sequences[axis] = std::shared_ptr<FixedSequence>(&*storage[axis], [](auto*){});
                if (!sequences[axis]->valid()) hal::panic("profile sequence");
                delays[axis] = 5ms + std::chrono::nanoseconds(std::uint64_t(sequences[axis]->firstTick()) * 100);
                if (!axes[axis]) axes[axis] = generator->output(static_cast<hal::step::Axis>(axis));
                if (!directions[axis]) directions[axis] = hal::core::create_stepper_direction_output(static_cast<hal::core::MotorId>(axis));
                directions[axis]->write(hal::gpio::Level::Low);
            }
            report.scheduling.add(now()-setup);
            if (!generator->setProgressCallback([&](const auto& s) noexcept {
                if (s.state == hal::step::State::Underrun || s.state == hal::step::State::DmaError ||
                    std::ranges::all_of(s.axes,[](auto x){return x==hal::step::State::Completed;})) complete.signal();
            })) hal::panic("profile callback");
            std::atomic_uint loadPeriods{};
            std::jthread load;
            if (r.fixture) load = runtime::thread::create_jthread(
                {.name="bench.reserve",.priority=3,.stack_size=4096},[&](std::stop_token stop){
                    static auto pin = hal::gpio::create_output({.pin={hal::gpio::Port::B,11}});
                    const auto burn = [](unsigned n){asm volatile("1: subs %0, %0, #1\n bne 1b" : "+r"(n) :: "cc");};
                    unsigned calibration;
                    { const hal::stm32::InterruptGuard guard; const auto start=cycles(); burn(1000); calibration=cycles()-start; }
                    const auto work = std::max<std::uint32_t>(1U,r.fixture*2'400'000U/calibration);
                    auto next=std::chrono::steady_clock::now();
                    while(!stop.stop_requested()) {
                        next+=1ms; pin->write(hal::gpio::Level::High); burn(work); pin->write(hal::gpio::Level::Low);
                        ++loadPeriods; std::this_thread::sleep_until(next);
                    }
                });
            // The selected half size determines both refill work and the
            // commitment horizon. This is a test setting, not a guarantee.
            giga_motion_bench_buffer_edges = 2*r.bytes;
            giga_step_irq_cycles_max = 0;
            giga_motion_bench_worst_refill_cycles = 0;
            giga_motion_bench_worst_budget_ticks = 0;
            release(shared.armed, acquire(shared.sequence));
            const auto started = now();
            for (unsigned repetition=0; repetition<r.count; ++repetition) {
                if (motorEnable->read()!=hal::gpio::Level::High) hal::panic("profile enable");
                const auto prepare = now();
                for (unsigned axis=0;axis<3;++axis)
                    if (!axes[axis]->prepareSequence(sequences[axis])) hal::panic("profile prepare");
                if (!generator->start() || !generator->startPrepared(delays)) hal::panic("profile start");
                report.processing.add(now()-prepare);
                const auto deadline=std::chrono::steady_clock::now()+2s;
                for (;;) {
                    const auto s=generator->status();
                    if (s.state==hal::step::State::Underrun || s.state==hal::step::State::DmaError ||
                        std::ranges::all_of(s.axes,[](auto x){return x==hal::step::State::Completed;}) ||
                        std::chrono::steady_clock::now()>=deadline) break;
                    complete.waitUntil(deadline);
                }
                const auto finalState=generator->status().state;
                const auto status=generator->stop();
                if(finalState==hal::step::State::Underrun || finalState==hal::step::State::DmaError)
                    report.rejected=std::to_underlying(finalState);
                for(unsigned axis=0;axis<3;++axis)
                    if(!status.counts_exact || status.pulses[axis]!=sequences[axis]->count()) ++report.errors;
                report.received+=status.pulses[0]; report.value+=status.pulses[1]; report.checksum+=status.pulses[2];
                ++report.sent;
                if(report.errors) break;
                std::this_thread::sleep_for(10ms);
            }
            report.elapsed=now()-started;
            if(load.joinable()){load.request_stop();load.join();}
            report.clock=loadPeriods;
            report.maximum_backlog=giga_step_irq_cycles_max;
            // These two profile-specific fields form a pair: 240 MHz core
            // cycles versus 10 MHz ticks left until the armed guard deadline.
            report.refill_cycles=giga_motion_bench_worst_refill_cycles;
            report.refill_budget_ticks=giga_motion_bench_worst_budget_ticks;
            static_cast<void>(generator->setProgressCallback({}));
            for(auto& axis:axes) static_cast<void>(axis->clear());
            giga_motion_bench_buffer_edges = 512;
            if(motorEnable->read()!=hal::gpio::Level::High) ++report.errors;
            tx_thread_priority_change(tx_thread_identify(), oldPriority, &oldPriority);
        }
        else {
            while(acquire(shared.armed)!=acquire(shared.sequence)) std::this_thread::sleep_for(1ms);
            auto state = ordinary.bus.topic<^^Topics::data>();
            auto log = ordinary.bus.topic<^^Topics::log>();
            const auto start=now();
            while(acquire(shared.done)!=acquire(shared.sequence)) {
                state.publish(std::string(512,'s')); log.publish(std::string(480,'b')); ++report.sent;
                if(now()-start > r.count*700000U + 5'000'000U) {++report.errors;break;}
                std::this_thread::sleep_for(10ms);
            }
            report.elapsed=now()-start;
        }
    }
}
