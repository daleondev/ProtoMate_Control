#include "Link.hpp"
#include "hal/hal.hpp"
#include "hal/stm32/InterruptGuard.hpp"
#include "runtime/synchronization/Notification.hpp"
#include "hal/devices/impl/Lan9253.hpp"

#include <exception>
extern "C" volatile std::uint32_t giga_step_irq_cycles_max;

namespace
{
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    using State = hal::step::State;
    using Counts = std::array<hal::step::PulseCount, 3>;

    class Bench final
    {
      public:
        Bench()
        {
            enable = hal::core::create_steppers_enable_output();
            generator = hal::core::create_stepper_generator();
            if (!enable || !generator) {
                hal::panic("STEP bench factories unavailable");
            }
            for (std::size_t i{}; i < axes.size(); ++i) {
                const auto id = static_cast<hal::core::MotorId>(i);
                axes[i] = hal::core::create_stepper_step_output(generator, id);
                directions[i] = hal::core::create_stepper_direction_output(id);
                if (!axes[i] || !directions[i] ||
                    !axes[i]->setCompletionCallback([this, i](const auto&) noexcept {
                    ++report.completions[i];
                    completed.signal();
                })) {
                    hal::panic("STEP bench axis construction failed");
                }
            }
            if (!generator->setProgressCallback([this](const auto& value) noexcept {
                ++report.progress;
                for (std::size_t i{}; i < axes.size(); ++i) {
                    if (value.axes[i] == State::Running) {
                        if (value.pulses[i] > 0U && value.pulses[i] < previous[i]) {
                            monotonic = false;
                        }
                        previous[i] = value.pulses[i];
                    }
                    else {
                        // Ready/Stopped may retain the preceding motion's count.
                        previous[i] = 0U;
                    }
                }
            })) {
                hal::panic("STEP bench callback registration failed");
            }
        }
        ~Bench()
        {
            static_cast<void>(generator->stop());
            for (auto& axis : axes) {
                static_cast<void>(axis->setCompletionCallback({}));
            }
            static_cast<void>(generator->setProgressCallback({}));
        }

        bench::Report run(bench::Command command)
        {
            static_cast<void>(generator->stop());
            for (auto& axis : axes) {
                static_cast<void>(axis->clear());
            }
            completed.clear();
            report = { .sequence = command.sequence, .test = command.test };
            previous = {};
            monotonic = true;
            bool ok = check(padsLow() && enable->read() == hal::gpio::Level::High, 1);
            if (ok) {
                switch (command.test) {
                    case 1:
                        ok = constant({ 1ms, 1ms, 1ms }, { 1, 2, 3 }) && start() && finish({ 1, 2, 3 });
                        break;
                    case 2:
                        ok = independent();
                        break;
                    case 3:
                    case 8:
                        ok = constant({ 10us, 10us, 10us }, { 100000, 100000, 100000 }) && start() &&
                             finish({ 100000, 100000, 100000 });
                        break;
                    case 4:
                        ok = update();
                        break;
                    case 5:
                        ok = restart();
                        break;
                    case 6:
                        ok = check(generator->start().has_value(), 2);
                        if (ok) {
                            // Test-only jump while all outputs are idle. Cross a real
                            // hardware wrap without requiring a seven-minute capture.
                            TIM2->CNT = 0xFFFFFFFEU - 50000U;
                            ok = constant({ 100us, 200us, 400us }, { 512, 512, 512 }) && start() &&
                                 finish({ 512, 512, 512 });
                        }
                        break;
                    case 7:
                        ok = underrun();
                        break;
                    case 9:
                        ok = profile();
                        break;
                    case 10:
                        ok = ethercat();
                        break;
                    default:
                        ok = check(false, 99);
                        break;
                }
            }
            const auto state = generator->status();
            for (std::size_t i{}; i < axes.size(); ++i) {
                report.pulses[i] += state.pulses[i];
            }
            static_cast<void>(generator->stop());
            ok =
              check(monotonic && state.counts_exact && padsLow() && enable->read() == hal::gpio::Level::High,
                    3) &&
              ok;
            report.passed = ok;
            report.service_cycles = giga_step_irq_cycles_max;
            // At 100 kHz one 512-edge DMA buffer lasts 2.56 ms. Require
            // service <=1.28 ms (2x headroom); the analyzer verifies edges.
            if (command.test == 3 || command.test == 8) {
                report.passed &= check(report.service_cycles <= 307200u, 70);
            }
            return report;
        }

      private:
        bool ethercat()
        {
            auto spi = hal::core::create_ethercat_spi();
            auto cs = hal::core::create_ethercat_chip_select();
            auto reset = hal::core::create_ethercat_reset();
            if (!check(spi && cs && reset, 80)) { return false; }
            hal::device::Lan9253 esc{*spi, *cs};
            if (!check(esc.identify().has_value(), 81)) { return false; }
            for (unsigned i = 0; i < 100000; ++i) {
                const auto value = esc.readSystemRegister(hal::device::Lan9253::BYTE_TEST_ADDRESS);
                if (!check(value && *value == hal::device::Lan9253::BYTE_TEST_VALUE, 82)) { return false; }
            }
            return true;
        }
        bool check(bool value, std::uint32_t code)
        {
            if (!value && !report.check) {
                report.check = code;
            }
            return value;
        }
        static bool padsLow()
        {
            return !(GPIOA->IDR & (GPIO_PIN_0 | GPIO_PIN_2 | GPIO_PIN_3));
        }
        bool constant(std::array<std::chrono::nanoseconds, 3> periods, Counts counts)
        {
            for (std::size_t i{}; i < axes.size(); ++i) {
                if (!check(axes[i]->prepare({ periods[i], 5us }, counts[i]).has_value(), 10)) {
                    return false;
                }
            }
            return true;
        }
        bool start()
        {
            return check(generator->start().has_value(), 11) &&
                   check(generator->startPrepared({ 1ms, 1ms, 1ms }).has_value(), 12);
        }
        bool finish(Counts counts)
        {
            const auto deadline = Clock::now() + 4s;
            for (;;) {
                const auto state = generator->status();
                if (state.state != State::Running) {
                    return check(false, 13);
                }
                bool done = true;
                for (const auto axis : state.axes) {
                    done &= axis == State::Completed;
                }
                if (done) {
                    return check(state.counts_exact && state.pulses == counts && padsLow(), 14);
                }
                if (!completed.waitUntil(deadline)) {
                    return check(false, 15);
                }
            }
        }
        bool independent()
        {
            if (!constant({ 1ms, 500us, 250us }, { 300, 400, 500 }) ||
                !check(generator->start().has_value() && axes[0]->start().has_value(), 20)) {
                return false;
            }
            std::this_thread::sleep_for(17ms);
            if (!check(axes[1]->start().has_value(), 21)) {
                return false;
            }
            std::this_thread::sleep_for(23ms);
            return check(axes[2]->start().has_value(), 22) && finish({ 300, 400, 500 });
        }
        bool update()
        {
            if (!constant({ 50us, 200us, 400us }, { 20000, 5000, 2500 }) || !start()) {
                return false;
            }
            std::this_thread::sleep_for(100ms);
            const auto changed = axes[0]->updateTiming({ 100us, 5us });
            if (!check(changed.has_value(), 30)) {
                return false;
            }
            report.changed_at = *changed;
            return finish({ 20000, 5000, 2500 });
        }
        bool restart()
        {
            if (!constant({ 100us, 200us, 100us }, { 5000, 5000, 5000 }) || !start()) {
                return false;
            }
            std::this_thread::sleep_for(40ms);
            const auto stop_started = DWT->CYCCNT;
            const auto stopped = axes[1]->stop();
            report.stop_cycles = DWT->CYCCNT - stop_started;
            report.pulses[1] = stopped.pulses;
            if (!check(stopped.state == State::Stopped && stopped.counts_exact && stopped.pulses > 0, 40)) {
                return false;
            }
            std::this_thread::sleep_for(20ms);
            return check(axes[1]->prepare({ 125us, 5us }, 777).has_value() && axes[1]->start().has_value(),
                         41) &&
                   finish({ 5000, 777, 5000 });
        }
        bool underrun()
        {
            if (!constant({ 10us, 20us, 40us }, { 100000, 50000, 25000 }) ||
                !check(generator->start().has_value(), 11)) {
                return false;
            }
            constexpr std::array irqs{
                DMA1_Stream0_IRQn, DMA1_Stream1_IRQn, DMA1_Stream2_IRQn, DMA1_Stream3_IRQn, TIM7_IRQn
            };
            for (const auto irq : irqs) {
                HAL_NVIC_DisableIRQ(irq);
            }
            __DSB();
            // Mask refill interrupts before arming any output. The calling
            // thread can be preempted after startPrepared returns, so masking
            // afterward would allow an arbitrary number of buffers to run.
            const bool started = check(generator->startPrepared({ 1ms, 1ms, 1ms }).has_value(), 12);
            if (started) {
                std::this_thread::sleep_for(20ms);
                report.guard_stopped = !(TIM2->CR1 & TIM_CR1_CEN);
            }
            for (const auto irq : irqs) {
                HAL_NVIC_EnableIRQ(irq);
            }
            if (!started) {
                return false;
            }
            const auto state = generator->status();
            return check(report.guard_stopped && state.state == State::Underrun && state.counts_exact &&
                           state.pulses == Counts{ 512, 256, 128 },
                         50);
        }
        bool profile()
        {
            std::vector<hal::step::Timing> sequence;
            for (unsigned i{}; i < 1000; ++i) {
                const auto phase = i < 500 ? i : 999 - i;
                sequence.push_back({ std::chrono::microseconds{ 500 - phase / 2 }, 5us });
            }
            for (auto& axis : axes) {
                if (!check(axis->prepareSequence(sequence).has_value(), 60)) {
                    return false;
                }
            }
            return start() && finish({ 1000, 1000, 1000 });
        }

        runtime::Notification completed;
        bench::Report report;
        Counts previous{};
        bool monotonic{ true };
        std::shared_ptr<hal::IDigitalOutput> enable;
        std::array<std::shared_ptr<hal::IDigitalOutput>, 3> directions;
        std::shared_ptr<hal::IStepGenerator> generator;
        std::array<std::shared_ptr<hal::IStepOutput>, 3> axes;
    };
}

int main()
{
    try {
        bench::Link link{ bench::Publisher::IO };
        Bench test;
        std::atomic_uint32_t received{}, errors{};
        auto load = link.bus.topic<^^bench::Topics::bench_load>().subscribe([&](const bench::Load& data) {
            ++received;
            if (data != bench::packet(data.sequence)) {
                ++errors;
            }
            link.bus.topic<^^bench::Topics::bench_echo>().publish(data);
        }, pnm::msg::SubscriptionOptions::latestOnly());
        auto worker = runtime::thread::create_jthread(
          { .name = "bench.load", .priority = 20, .stack_size = 8192 }, [&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                load.poll(10ms);
            }
        });
        auto commands =
          link.bus.topic<^^bench::Topics::bench_command>().subscribe([&](const bench::Command& command) {
            const auto before = received.load();
            const auto prior_errors = errors.load();
            auto result = test.run(command);
            result.load_received = received.load() - before;
            result.load_errors = errors.load() - prior_errors;
            // Latest-only subscriptions may coalesce during CPU saturation. Require
            // verified bidirectional traffic during motion, not lossless delivery.
            result.passed &= result.load_errors == 0 && (command.test != 8 || result.load_received >= 10);
            link.bus.topic<^^bench::Topics::bench_report>().publish(result);
        });
        for (;;) {
            commands.poll(1s);
        }
    } catch (const std::exception& error) {
        hal::panic("M4 STEP bench", error.what());
    }
}
