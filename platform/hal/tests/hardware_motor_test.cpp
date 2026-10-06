#include "motor_bench/Commands.hpp"

#include "hal/board/board.hpp"
#include "hal/devices/impl/IndexFeedback.hpp"
#include "hal/devices/impl/Tmc2209.hpp"
#include "hal/hal.hpp"
#include "hal/stm32/InterruptGuard.hpp"
#include "runtime/synchronization/Notification.hpp"
#include "runtime/thread.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

// Debugger-visible result. Break only after completion, with both drivers disabled.
extern "C" {
volatile std::uint32_t hardware_motor_test_status{};
volatile std::uint32_t hardware_motor_test_pulses[2]{};
[[gnu::noinline, gnu::used]] void hardware_motor_test_complete() { asm volatile("" ::: "memory"); }
}

namespace
{
    using namespace std::chrono_literals;
    using hal::gpio::Level;
    using Motor = hal::board::MotorId;
    using Driver = hal::device::Tmc2209;
    using Clock = std::chrono::steady_clock;
    using motor_bench::Command;
    using motor_bench::Operation;
    constexpr Driver::Configuration configuration{
        .run_milliamps = 550, .hold_milliamps = 550, .microsteps = 16,
        .mode = Driver::Mode::StealthChop, .interpolate = true, .index_step = false
    };
    constexpr std::array motors{ Motor::Motor1, Motor::Motor2 };
    constexpr unsigned alarmFault = 1, diagFault = 2;
    static_assert(std::atomic_uint::is_always_lock_free && std::atomic_bool::is_always_lock_free);

    template<typename... Args>
    void log(const char* format, Args... args)
    {
        std::array<char, 384> message{};
        std::snprintf(message.data(), message.size(), format, args...);
        std::printf("[motor-test] %s\n", message.data());
        std::fflush(stdout);
    }

    void require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    template<typename T>
    void require(const hal::util::Result<T>& result, const char* message)
    {
        if (!result) throw std::runtime_error(std::string{ message } + ": " + result.error().message());
    }

    class Bench final
    {
        struct Feedback
        {
            std::int64_t encoder{}, cycles{};
            std::chrono::nanoseconds encoder_time{};
            double encoder_velocity{}, encoder_peak{}, index_velocity{}, index_positive{}, index_negative{};
            std::uint32_t encoder_samples{}, index_samples{}, z_edges{};
            bool encoder_valid{}, encoder_running{}, index_valid{};
        };

      public:
        Bench()
        {
            try {
                m_enable = hal::board::createSteppersEnableOutput();
                require(bool(m_enable), "shared enable unavailable");
                m_enable->write(Level::High);
                m_generator = hal::board::createStepperGenerator();
                require(bool(m_generator), "STEP generator unavailable");
                for (unsigned i{}; i < motors.size(); ++i) {
                    m_steps[i] = hal::board::createStepperStepOutput(m_generator, motors[i]);
                    m_dirs[i] = hal::board::createStepperDirectionOutput(motors[i]);
                    m_faultInputs[i] = hal::board::createStepperDiagnostic(motors[i]);
                    require(m_steps[i] && m_dirs[i] && m_faultInputs[i], "motor resources unavailable");
                }
                m_button = hal::board::createButton(hal::board::ButtonId::User);
                m_bus = hal::board::createStepperDriverBus();
                m_encoder = hal::board::createEncoder(Motor::Motor1);
                m_z = hal::board::createEncoderIndex(Motor::Motor1);
                require(m_button && m_bus && m_encoder && m_z, "bench resources unavailable");
                m_driver = std::make_unique<Driver>(m_bus, 0);
                m_index = std::make_unique<hal::device::IndexFeedback>(hal::board::createStepperIndex(Motor::Motor2));
                for (unsigned i{}; i < motors.size(); ++i) {
                    require(m_steps[i]->setCompletionCallback([this](const auto&) noexcept {
                        m_notification.signal();
                    }), "completion callback");
                    m_faultInputs[i]->setEdgeCallback([this, i](Level level) noexcept {
                        if (level == Level::High) trip(1U << i);
                    });
                }
                m_button->setStateChangedCallback([this](auto state) noexcept {
                    if (state == hal::device::IButton::State::Pressed) cancel();
                });
                m_encoder->setSampleCallback([this](const hal::IQuadratureEncoder::Sample& sample) noexcept {
                    // IRQ and thread publications use the same short critical section.
                    const hal::stm32::InterruptGuard guard;
                    auto& f = m_feedback;
                    f.encoder_velocity = 0;
                    if (sample.position && sample.running && f.encoder_valid && f.encoder_running &&
                        sample.timestamp > f.encoder_time)
                        f.encoder_velocity = (double(*sample.position) - double(f.encoder)) /
                                             std::chrono::duration<double>(sample.timestamp - f.encoder_time).count();
                    f.encoder_peak = std::max(f.encoder_peak, std::abs(f.encoder_velocity));
                    f.encoder_valid = bool(sample.position);
                    f.encoder_running = sample.running;
                    if (sample.position) f.encoder = *sample.position;
                    f.encoder_time = sample.timestamp;
                    ++f.encoder_samples;
                });
                m_z->setEdgeCallback([this](Level level) noexcept {
                    const hal::stm32::InterruptGuard guard;
                    if (level == Level::High) ++m_feedback.z_edges;
                });
                m_index->setCallback([this](const hal::device::IndexFeedback::Sample& sample) noexcept {
                    const hal::stm32::InterruptGuard guard;
                    auto& f = m_feedback;
                    f.index_valid = bool(sample.cycles);
                    if (sample.cycles) f.cycles = *sample.cycles;
                    f.index_velocity = sample.cycles_per_second;
                    f.index_positive = std::max(f.index_positive, f.index_velocity);
                    f.index_negative = std::min(f.index_negative, f.index_velocity);
                    ++f.index_samples;
                });
                require(m_steps[1]->setProgressCallback([this](const hal::step::AxisStatus& state) noexcept {
                    const auto period = state.period.count() > 0 ? state.period :
                                        std::chrono::nanoseconds{ m_m2Period.load() };
                    m_index->motion(state.state == hal::step::State::Running, m_m2Forward.load(), period * 64);
                }), "INDEX progress callback");
                require(m_encoder->start(), "start M1 encoder sampling");
                require(m_generator->start(), "start idle STEP timebase");
                if (const auto faults = levels()) trip(faults);
            } catch (...) {
                quiesce();
                detach();
                throw;
            }
        }

        ~Bench()
        {
            cancel();
            if (m_worker.joinable()) m_worker.join();
            quiesce();
            detach();
        }

        void cancel() noexcept
        {
            // Fault/B1 ISR path: no UART, logging, allocation or generator reentry.
            m_abort.store(true);
            m_ready.store(false);
            if (m_enable) m_enable->write(Level::High);
            m_notification.signal();
        }

        void command(std::string_view line)
        {
            const auto parsed = motor_bench::parse(line);
            if (!parsed) {
                log("REJECTED: invalid command/arguments; type help. No action taken.");
                return;
            }
            const auto cmd = *parsed;
            if (cmd.operation == Operation::Stop) {
                cancel();
                log("STOP: shared EN high; worker stops STEP. Run check before further motion.");
                return;
            }
            if (cmd.operation == Operation::Help) { menu(); return; }
            if (cmd.operation == Operation::Status) { status(); return; }
            if (m_busy.load()) { log("BUSY: status, stop and B1 remain available."); return; }
            if (m_worker.joinable()) m_worker.join();
            if (cmd.operation == Operation::Check) {
                // Acknowledge old faults HERE, never later in the worker where
                // doing so could erase a stop/edge received after the command.
                bool healthy{};
                {
                    const hal::stm32::InterruptGuard guard;
                    healthy = levels() == 0 && !m_button->isPressed();
                    m_ready.store(false);
                    if (healthy) {
                        m_abort.store(false);
                        m_faults.store(0);
                    }
                }
                if (!healthy) { log("REJECTED: release B1; M1 ALM and M2 DIAG must both be LOW."); status(); return; }
            }
            else if (!m_ready.load()) { log("REJECTED: check must pass first."); return; }
            m_busy.store(true);
            try {
                m_worker = runtime::thread::create_jthread({ .name = "M1 M2 bench", .stack_size = 16384 },
                                                           [this, cmd] { run(cmd); });
            } catch (...) {
                cancel();
                quiesce();
                m_busy.store(false);
                throw;
            }
        }

        static void menu()
        {
            log("check | status | hold | stop | help");
            log("move m1|m2|both [pulses [hz]] = out and back; defaults 400 STEP, 200 Hz");
            log("forward|reverse m1|m2|both [pulses [hz]] = one leg; forward DIR=high");
            log("Limits: 1..3200 pulses, 50..1000 Hz, <=20 s/leg. STEP high time=5 us.");
            log("independent = move/reverse M2 while M1 continues, then return M1");
            log("feedback m1|m2|both = verify M1 encoder / M2 electrical INDEX with bounded moves");
            log("SHARED ENABLE: every hold/move energizes BOTH drivers. M3 must be unpowered.");
            log("M1: DM542T DIP 1/16 (3200 STEP/rev). M2: address 0, StealthChop, 550 mA requested.");
            log("ALM PF2 and DIAG PD4 required. Feedback wiring is required only for feedback tests.");
            log("No homing/limit-switch protection: use secured, unloaded motors. B1 or stop aborts.");
        }

      private:
        void detach() noexcept
        {
            for (auto& step : m_steps) if (step) {
                static_cast<void>(step->setProgressCallback({}));
                static_cast<void>(step->setCompletionCallback({}));
            }
            if (m_index) m_index->clearCallback();
            if (m_encoder) { m_encoder->clearSampleCallback(); static_cast<void>(m_encoder->stop()); }
            if (m_z) m_z->clearEdgeCallback();
            for (auto& input : m_faultInputs) if (input) input->clearEdgeCallback();
            if (m_button) m_button->setStateChangedCallback({});
            if (m_generator) static_cast<void>(m_generator->stop());
        }

        void trip(unsigned faults) noexcept { m_faults.fetch_or(faults); cancel(); }
        unsigned levels() const noexcept
        {
            return (m_faultInputs[0]->read() == Level::High ? alarmFault : 0U) |
                   (m_faultInputs[1]->read() == Level::High ? diagFault : 0U);
        }
        void active()
        {
            if (const auto faults = levels()) trip(faults);
            const auto faults = m_faults.load();
            require(!(faults & alarmFault), "M1 DM542T ALM fault/open cable; inspect red LED and wiring");
            require(!(faults & diagFault), "M2 TMC2209 DIAG fault");
            require(!m_abort.load() && !m_button->isPressed(), "stopped by console/B1; run check again");
        }
        void quiesce() noexcept
        {
            if (m_enable) m_enable->write(Level::High);
            for (auto& step : m_steps) if (step) static_cast<void>(step->stop());
            if (m_index) m_index->invalidate();
        }
        Feedback feedback() const noexcept
        {
            const hal::stm32::InterruptGuard guard;
            return m_feedback;
        }
        void resetPeaks() noexcept
        {
            const hal::stm32::InterruptGuard guard;
            m_feedback.encoder_peak = m_feedback.index_positive = m_feedback.index_negative = 0;
        }
        static bool stepsLow() noexcept
        {
            return (GPIOA->IDR & GPIO_PIN_0) == 0 && (GPIOB->IDR & (GPIO_PIN_10 | GPIO_PIN_11)) == 0;
        }
        void status()
        {
            const auto a = m_steps[0]->status(), b = m_steps[1]->status();
            const auto f = feedback();
            log("EN=%s busy=%u qualified=%u latched=%u (M1=1,M2=2) ALM=%u DIAG=%u",
                m_enable->read() == Level::High ? "disabled" : "enabled", unsigned(m_busy.load()),
                unsigned(m_ready.load()), m_faults.load(), unsigned(m_faultInputs[0]->read() == Level::High),
                unsigned(m_faultInputs[1]->read() == Level::High));
            log("STEP counts: M1=%llu state=%u exact=%u; M2=%llu state=%u exact=%u",
                static_cast<unsigned long long>(a.pulses), unsigned(a.state), unsigned(a.counts_exact),
                static_cast<unsigned long long>(b.pulses), unsigned(b.state), unsigned(b.counts_exact));
            log("M1 encoder: valid=%u count=%lld velocity=%.2f deg/s samples=%lu Z_edges=%lu",
                unsigned(f.encoder_valid), static_cast<long long>(f.encoder), f.encoder_velocity * 360.0 / 1600.0,
                static_cast<unsigned long>(f.encoder_samples), static_cast<unsigned long>(f.z_edges));
            log("M2 electrical INDEX: valid=%u cycles=%lld velocity=%.2f deg/s; NOT shaft feedback",
                unsigned(f.index_valid), static_cast<long long>(f.cycles), f.index_velocity * 7.2);
        }
        Driver::Status sample(bool enabled)
        {
            active();
            const auto state = m_driver->status();
            require(state, "M2 UART diagnostics");
            require(!state->reset() && !state->fault() && !state->warning(), "M2 reset/fault/thermal warning");
            require(bool(state->input & 1U) != enabled, "M2 ENN readback differs from shared enable");
            require(m_driver->verify(), "M2 configuration readback");
            active();
            return *state;
        }
        std::uint32_t reg(std::uint8_t address)
        {
            std::array<std::uint8_t, 4> request{ 5, 0, address, 0 };
            request.back() = Driver::crc(std::span{ request }.first(3));
            std::array<std::uint8_t, 8> reply{};
            require(m_bus->exchange(request, reply, 10ms), "M2 register read");
            require(reply[0] == 5 && reply[1] == 0xFF && reply[2] == address &&
                    Driver::crc(std::span{ reply }.first(7)) == reply.back(), "M2 register CRC/header");
            return (std::uint32_t{ reply[3] } << 24U) | (std::uint32_t{ reply[4] } << 16U) |
                   (std::uint32_t{ reply[5] } << 8U) | reply[6];
        }
        void dwell(std::chrono::milliseconds time, bool enabled)
        {
            const auto end = Clock::now() + time;
            do {
                static_cast<void>(sample(enabled));
                require(stepsLow(), "STEP must be low during a stationary interval");
                m_notification.waitUntil(std::min(end, Clock::now() + 20ms));
            } while (Clock::now() < end);
            active();
        }
        void check()
        {
            active();
            static_cast<void>(m_generator->stop());
            for (auto& step : m_steps) require(step->clear(), "clear STEP schedule");
            require(m_generator->start(), "restart idle timebase");
            constexpr std::array<std::uint8_t, 1> addresses{ 0 };
            require(Driver::prepareBus(*m_bus, addresses), "prepare M2 UART");
            require(m_driver->initialize(configuration), "configure M2 StealthChop");
            dwell(200ms, false);
            const auto state = sample(false);
            log("M2 verified: address=0, 1/16, StealthChop, nominal %u mA RMS; GSTAT=%08lx DRV_STATUS=%08lx",
                unsigned(Driver::currentMilliamps(*Driver::currentScale(configuration.run_milliamps))),
                static_cast<unsigned long>(state.global), static_cast<unsigned long>(state.driver));
            log("M1 ALM healthy; STEP pads low. DIP current/microsteps, winding and encoder wiring are NOT readable.");
            {
                const hal::stm32::InterruptGuard guard;
                if (!m_abort.load() && !m_faults.load() && !levels() && !m_button->isPressed()) m_ready.store(true);
            }
            active();
            require(m_ready.load(), "qualification interrupted");
        }
        void enable()
        {
            log("Both drivers will energize in 2 seconds. stop/B1 aborts.");
            dwell(2s, false);
            bool enabled{};
            {
                const hal::stm32::InterruptGuard guard;
                if (m_ready.load() && !m_abort.load() && !m_faults.load() && !levels() && !m_button->isPressed()) {
                    m_enable->write(Level::Low);
                    enabled = true;
                }
            }
            require(enabled, "enable interrupted");
            // Covers the DM542T's 200 ms enable time and TMC standstill auto-tuning.
            dwell(1s, true);
            const auto state = sample(true);
            require(state.driver & (1U << 30U), "M2 did not enter StealthChop");
            require(((state.driver >> 16U) & 31U) == *Driver::currentScale(configuration.run_milliamps),
                    "M2 current scale differs from configuration");
        }
        void hold()
        {
            const auto phase = reg(0x6A), writes = reg(0x02);
            const auto origin = m_encoder->position();
            log("HOLD both for 5 s; no STEP pulses. M1 ALM and M2 DIAG monitored.");
            dwell(5s, true);
            require(sample(true).standstill(), "M2 must report standstill during hold");
            require(reg(0x6A) == phase && reg(0x02) == writes, "M2 phase/configuration changed during hold");
            const auto end = m_encoder->position();
            if (origin && end) log("M1 encoder change during hold: %lld counts (unconnected encoder can also read zero)",
                                   static_cast<long long>(*end - *origin));
        }
        void prepare(unsigned axis, bool forward, std::uint32_t pulses, std::uint32_t hertz)
        {
            active();
            m_dirs[axis]->write(forward ? Level::High : Level::Low);
            const auto period = std::chrono::nanoseconds{ (1'000'000'000ULL + hertz - 1) / hertz };
            if (axis == 1) { m_m2Forward.store(forward); m_m2Period.store(static_cast<unsigned>(period.count())); }
            require(m_steps[axis]->prepare({ period, 5us }, pulses), "prepare finite STEP output");
        }
        void await(unsigned axes, const std::array<std::uint32_t, 2>& counts, Clock::time_point deadline)
        {
            for (;;) {
                active();
                bool done = true;
                for (unsigned i{}; i < motors.size(); ++i) {
                    const auto state = m_steps[i]->status();
                    require(state.state != hal::step::State::Underrun && state.state != hal::step::State::DmaError,
                            "STEP generator fault");
                    if (!(axes & (1U << i))) continue;
                    require(state.counts_exact, "STEP count uncertain");
                    if (state.state == hal::step::State::Completed) {
                        require(state.pulses == counts[i], "wrong completed STEP count");
                        hardware_motor_test_pulses[i] = static_cast<std::uint32_t>(state.pulses);
                    }
                    else {
                        require(state.state == hal::step::State::Running, "STEP output stopped unexpectedly");
                        done = false;
                    }
                }
                static_cast<void>(sample(true));
                if (done) break;
                require(Clock::now() < deadline, "motion deadline exceeded");
                m_notification.waitUntil(std::min(deadline, Clock::now() + 20ms));
            }
            active();
        }
        void leg(unsigned axes, bool forward, std::uint32_t pulses, std::uint32_t hertz)
        {
            const auto phase = reg(0x6A);
            const auto encoder = m_encoder->position();
            const std::array before{ m_steps[0]->status(), m_steps[1]->status() };
            std::array<std::optional<std::chrono::nanoseconds>, 3> delays{};
            for (unsigned i{}; i < motors.size(); ++i) if (axes & (1U << i)) {
                prepare(i, forward, pulses, hertz);
                delays[i] = 2ms;
            }
            log("LEG axes=%u DIR=%s pulses=%lu frequency=%lu Hz", axes, forward ? "high" : "low",
                static_cast<unsigned long>(pulses), static_cast<unsigned long>(hertz));
            active();
            require(m_generator->startPrepared(delays), "start selected axes");
            await(axes, { pulses, pulses }, Clock::now() + std::chrono::milliseconds{ (pulses * 1000ULL + hertz - 1) / hertz } + 1s);
            dwell(200ms, true); // Interpolation and shaft settling before endpoint checks.
            for (unsigned i{}; i < motors.size(); ++i) if (!(axes & (1U << i))) {
                const auto after = m_steps[i]->status();
                require(after.counts_exact && after.pulses == before[i].pulses && after.state == before[i].state,
                        "unselected STEP output changed");
            }
            const auto delta = (axes & 2U) ? (pulses * 16U) % 1024U : 0U;
            const auto expected = (phase + (forward ? 1024U - delta : delta)) % 1024U;
            require(reg(0x6A) == expected, "M2 electrical phase mismatch / unselected M2 moved");
            const auto end = m_encoder->position();
            if (encoder && end) log("M1 measured delta=%lld encoder counts; expected magnitude=%lu when M1 selected at 1/16",
                                    static_cast<long long>(*end - *encoder), static_cast<unsigned long>((axes & 1U) ? pulses / 2U : 0U));
            log("LEG complete: exact commanded pulses; M2 MSCNT verified. Shaft/INDEX verification uses feedback commands.");
        }
        void independent()
        {
            // M2 completes AND reverses while the same M1 motion is still active.
            prepare(0, true, 800, 200);
            require(m_steps[0]->start(2ms), "start M1 independently");
            const auto started = Clock::now();
            while (Clock::now() - started < 300ms) {
                static_cast<void>(sample(true));
                m_notification.waitUntil(Clock::now() + 20ms);
            }
            std::uint64_t previous{};
            const auto phase = reg(0x6A);
            for (bool forward : { true, false }) {
                const auto first = m_steps[0]->status();
                require(first.state == hal::step::State::Running && first.pulses >= previous,
                        "M1 stopped/reset before independent M2 start");
                prepare(1, forward, 400, 400);
                require(m_steps[1]->start(2ms), "start M2 while M1 runs");
                await(2, { 800, 400 }, Clock::now() + 2s);
                const auto current = m_steps[0]->status();
                require(current.state == hal::step::State::Running && current.pulses > first.pulses,
                        "M1 was interrupted by M2 motion");
                previous = current.pulses;
                log("M2 leg completed; M1 continues with %llu pulses", static_cast<unsigned long long>(previous));
            }
            await(1, { 800, 400 }, started + 5s);
            dwell(300ms, true);
            require(reg(0x6A) == phase, "M2 did not return to its original electrical phase");
            leg(1, false, 800, 200);
            log("Independent starts/completions/reversal verified; M1 returned by commanded pulses.");
        }
        std::int64_t encoderCount()
        {
            const auto value = m_encoder->position();
            require(value, "M1 encoder count");
            return *value;
        }
        void encoderTest()
        {
            const auto origin = encoderCount();
            resetPeaks();
            leg(1, true, 400, 200);
            const auto outward = encoderCount() - origin;
            const auto peak = feedback().encoder_peak;
            log("ENCODER outward=%lld expected magnitude=200 +/-4; peak=%.2f deg/s",
                static_cast<long long>(outward), peak * 360.0 / 1600.0);
            require(std::abs(double(outward)) >= 196 && std::abs(double(outward)) <= 204 && peak > 0,
                    "M1 encoder movement mismatch: check A/B, 1/16 DIP setting, coupling and motor motion");
            leg(1, false, 400, 200);
            const auto residual = encoderCount() - origin;
            require(std::abs(double(residual)) <= 4, "M1 encoder did not return within 4 counts");
            log("M1 ENCODER verified: DIR-high count sign=%s; return error=%lld counts. Z is reported by status separately.",
                outward > 0 ? "positive" : "negative", static_cast<long long>(residual));
        }
        void indexTest()
        {
            leg(2, true, 128, 400); // Establish electrical phase; not an absolute shaft origin.
            auto f = feedback();
            require(f.index_valid, "no M2 INDEX feedback: connect INDEX to PD0/CN9.25");
            const auto origin = f.cycles;
            resetPeaks();
            leg(2, true, 256, 100);
            f = feedback();
            require(f.index_valid && f.cycles == origin + 4 && f.index_velocity == 0 &&
                    f.index_positive > 1.45 && f.index_positive < 1.7, "M2 forward INDEX count/velocity mismatch");
            leg(2, false, 256, 400);
            f = feedback();
            require(f.index_valid && f.cycles == origin && f.index_velocity == 0 &&
                    f.index_negative < -5.9 && f.index_negative > -6.6, "M2 reverse INDEX count/velocity mismatch");
            leg(2, true, 16, 100);
            leg(2, false, 16, 100);
            f = feedback();
            require(f.index_valid && f.cycles == origin, "M2 short reversal INDEX drift");
            log("M2 electrical INDEX verified: 64 STEP/cycle; peaks=%.3f/%.3f cycles/s; NOT measured rotor position.",
                f.index_positive, f.index_negative);
            leg(2, false, 128, 400); // Undo the phase-establishing excursion too.
        }
        void run(Command cmd) noexcept
        {
            hardware_motor_test_status = 0x52554E00U;
            hardware_motor_test_pulses[0] = 0;
            hardware_motor_test_pulses[1] = 0;
            bool passed{};
            try {
                quiesce();
                log("BEGIN %s; shared EN disabled", motor_bench::name(cmd.operation));
                if (cmd.operation == Operation::Check) check();
                else {
                    enable();
                    if (cmd.operation == Operation::Hold) hold();
                    else if (cmd.operation == Operation::Independent) independent();
                    else if (cmd.operation == Operation::Feedback) {
                        if (cmd.axes & 1U) encoderTest();
                        if (cmd.axes & 2U) indexTest();
                    }
                    else {
                        leg(cmd.axes, cmd.operation != Operation::Reverse, cmd.pulses, cmd.hertz);
                        if (cmd.operation == Operation::Move) { dwell(300ms, true); leg(cmd.axes, false, cmd.pulses, cmd.hertz); }
                    }
                    quiesce();
                    static_cast<void>(sample(false));
                }
                active();
                passed = true;
            } catch (const std::exception& error) {
                cancel();
                quiesce();
                log("FAIL: %s", error.what());
            } catch (...) {
                cancel();
                quiesce();
                log("FAIL: unexpected exception");
            }
            quiesce();
            hardware_motor_test_status = passed ? 0x600D600DU : 0xBAD00000U | unsigned(cmd.operation);
            hardware_motor_test_complete();
            if (passed) log("PASS: requested checks completed; both drivers disabled.");
            m_busy.store(false);
            log("READY; status/help available. Failed/stopped operations require check.");
        }

        std::shared_ptr<hal::IDigitalOutput> m_enable;
        std::shared_ptr<hal::IStepGenerator> m_generator;
        std::array<std::shared_ptr<hal::IStepOutput>, 2> m_steps;
        std::array<std::shared_ptr<hal::IDigitalOutput>, 2> m_dirs;
        std::array<std::shared_ptr<hal::IDigitalInput>, 2> m_faultInputs;
        std::shared_ptr<hal::device::IButton> m_button;
        std::shared_ptr<hal::IUart> m_bus;
        std::unique_ptr<Driver> m_driver;
        std::shared_ptr<hal::IQuadratureEncoder> m_encoder;
        std::shared_ptr<hal::IDigitalInput> m_z;
        std::unique_ptr<hal::device::IndexFeedback> m_index;
        Feedback m_feedback{};
        std::atomic_uint m_faults{}, m_m2Period{ 5'000'000 };
        std::atomic_bool m_abort{}, m_ready{}, m_busy{}, m_m2Forward{};
        runtime::Notification m_notification;
        std::jthread m_worker;
    };
}

int main()
{
    try {
        Bench bench;
        log("READY: M1 DM542T + M2 TMC2209; StealthChop; EN disabled; no startup motion");
        Bench::menu();
        std::array<char, 96> line{};
        std::size_t size{};
        bool overflow{}, carriage_return{};
        for (;;) {
            const auto c = std::getchar();
            require(c != EOF, "console disconnected");
            if (c == '\0') continue;
            if (c == 3) { bench.cancel(); size = 0; overflow = false; continue; }
            if (c == '\n' && carriage_return) { carriage_return = false; continue; }
            carriage_return = c == '\r';
            if (c == '\n' || c == '\r') {
                if (overflow) log("REJECTED: line too long; no action taken");
                else bench.command({ line.data(), size });
                size = 0;
                overflow = false;
            }
            else if (!overflow && (c == '\b' || c == 127)) { if (size) --size; }
            else if (!overflow) {
                if (size == line.size()) overflow = true;
                else line[size++] = static_cast<char>(c);
            }
        }
    } catch (const std::exception& error) {
        log("FATAL: %s", error.what());
        return 1;
    }
}
