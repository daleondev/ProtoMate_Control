#include "hal/board/board.hpp"
#include "hal/devices/impl/Tmc2209.hpp"
#include "hal/devices/impl/IndexFeedback.hpp"
#include "hal/drivers/common.hpp"
#include "hal/hal.hpp"
#include "hal/stm32/InterruptGuard.hpp"
#include "runtime/synchronization/Notification.hpp"
#include "runtime/thread.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <thread>

// Break here only AFTER a case finishes; do not halt during enabled motion.
extern "C" {
volatile std::uint32_t hardware_tmc_test_status{};
volatile std::uint32_t hardware_tmc_test_pulses[2]{};
[[gnu::noinline, gnu::used]] void hardware_tmc_test_complete() { asm volatile("" ::: "memory"); }
}

namespace
{
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    using Driver = hal::device::Tmc2209;
    using hal::gpio::Level;
    constexpr auto motor = hal::board::MotorId::Motor2;
    // Both modes use CS=8 (nominal 511 mA), within StealthChop's IRUN range.
    constexpr Driver::Configuration benchConfig{ .run_milliamps = 550, .hold_milliamps = 550 };
    static_assert(std::atomic_bool::is_always_lock_free);

    const char* modeName(Driver::Mode mode)
    {
        return mode == Driver::Mode::StealthChop ? "StealthChop" : "SpreadCycle";
    }

    template<typename... Args>
    void log(const char* format, Args... args)
    {
        std::printf("[tmc-test] ");
        std::printf(format, args...);
        std::printf("\n");
        std::fflush(stdout);
    }

    void require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    template<typename T>
    void require(const hal::util::Result<T>& result, const char* message)
    {
        if (!result)
            throw std::runtime_error(std::string{ message } + ": " + result.error().message());
    }

    class Bench final
    {
      public:
        Bench()
        {
            m_enable = hal::board::createSteppersEnableOutput();
            require(bool(m_enable), "enable output unavailable");
            m_enable->write(Level::High);
            m_generator = hal::board::createStepperGenerator();
            require(bool(m_generator), "step generator unavailable");
            m_step = hal::board::createStepperStepOutput(m_generator, motor);
            m_dir = hal::board::createStepperDirectionOutput(motor);
            m_diag = hal::board::createStepperDiagnostic(motor);
            m_button = hal::board::createButton(hal::board::ButtonId::User);
            m_bus = hal::board::createStepperDriverBus();
            require(m_step && m_dir && m_diag && m_button && m_bus, "bench resources unavailable");
            m_driver = std::make_unique<Driver>(m_bus, m_address);
            require(m_step->setCompletionCallback(
                      [this](const hal::step::AxisStatus&) noexcept { m_notification.signal(); }),
                    "completion callback");
            require(m_generator->start(), "start idle step timebase");
            m_diag->setEdgeCallback([this](Level level) noexcept {
                if (level == Level::High) {
                    m_fault.store(true);
                    cancel();
                }
            });
            m_button->setStateChangedCallback([this](hal::device::IButton::State state) noexcept {
                if (state == hal::device::IButton::State::Pressed)
                    cancel();
            });
        }

        ~Bench()
        {
            m_diag->setEdgeCallback({});
            m_button->setStateChangedCallback({});
            cancel();
            if (m_worker.joinable())
                m_worker.join();
            static_cast<void>(m_step->setCompletionCallback({}));
            static_cast<void>(m_generator->stop());
        }

        void cancel() noexcept
        {
            // Also called by B1/DIAG interrupts: never touch UART or take a mutex.
            m_abort.store(true);
            m_ready.store(false);
            m_enable->write(Level::High);
            m_notification.signal();
        }

        void command(std::string_view command)
        {
            if (command == "stop") {
                cancel();
                log("STOP requested; EN high. Run check before another motion.");
                return;
            }
            if (m_busy.load()) {
                log("BUSY; EN=%s fault=%u. stop or B1 aborts; other commands rejected.",
                    m_enable->read() == Level::High ? "disabled" : "enabled",
                    unsigned(m_fault.load()));
                return;
            }
            if (m_worker.joinable())
                m_worker.join();
            if (command == "mode stealth" || command == "mode spread") {
                quiesce();
                m_ready.store(false);
                m_configuration.mode =
                  command == "mode stealth" ? Driver::Mode::StealthChop : Driver::Mode::SpreadCycle;
                log("Selected %s; EN disabled. Run check to apply and verify.",
                    modeName(m_configuration.mode));
                return;
            }
            if (command == "address 0" || command == "address 1") {
                quiesce();
                m_ready.store(false);
                m_address = command.back() - '0';
                m_driver = std::make_unique<Driver>(m_bus, m_address);
                log("Selected address %u; STEP/DIR/DIAG remain on M2 pins. Run check.", unsigned(m_address));
                return;
            }
            if (command != "check" && command != "status" && command != "hold" && command != "move" &&
                command != "index" && command != "feedback") {
                menu();
                return;
            }
            if ((command == "hold" || command == "move" || command == "index" || command == "feedback") && !m_ready.load()) {
                log("REJECTED: check must pass before enabling the motor.");
                return;
            }
            if ((command == "index" || command == "feedback") && m_configuration.mode != Driver::Mode::StealthChop) {
                log("REJECTED: use mode stealth, then check, before index.");
                return;
            }
            // The selected operation outlives the input buffer; capture a value.
            const auto operation = command == "check"    ? 0
                                   : command == "status" ? 1
                                   : command == "hold"   ? 2
                                   : command == "move"   ? 3
                                   : command == "index"  ? 4 : 5;
            if (operation == 0) {
                const hal::stm32::InterruptGuard guard;
                m_abort.store(false);
                m_fault.store(false);
            }
            m_busy.store(true);
            try {
                m_worker = runtime::thread::create_jthread({ .name = "TMC bench", .stack_size = 16384 },
                                                           [this, operation] { run(operation); });
            } catch (...) {
                quiesce();
                m_busy.store(false);
                throw;
            }
        }

        static void menu()
        {
            log("check = disabled UART/configuration suite; status = read diagnostics (no enable)");
            log("mode stealth|spread = select chopper while disabled; run check after selecting");
            log("hold = settle 1 s then hold 5 s without STEP; move = +400 then -400 pulses, 400 Hz / 5 us "
                "high");
            log("index = three external-STEP/INDEX phases; StealthChop required; capture D0 and D7");
            log("feedback = normal INDEX callback test; connect INDEX to PD0/CN9.25; D0 STEP, D1 INDEX");
            log("stop = disable/abort; address 0|1 = select strapped node; help = this menu");
            log("Only ONE TMC2209. Physical M2 pins for either address; no robot/perfboard required.");
            log("Both modes: 550 mA RMS requested (nominal 511 mA), run=hold, 16 microsteps, interpolation "
                "on.");
            log(
              "Startup selection: SpreadCycle. Compare mode spread/check/hold with mode stealth/check/hold.");
            log("B1 disables immediately. No automatic retry, enable or startup motion.");
        }

      private:
        void quiesce() noexcept
        {
            m_enable->write(Level::High);
            static_cast<void>(m_step->stop());
        }

        void active() const
        {
            require(!m_abort.load() && !m_button->isPressed(), "aborted by stop/B1; run check to recover");
            require(!m_fault.load() && m_diag->read() == Level::Low, "DIAG fault; EN disabled");
        }

        auto sample(bool enabled) -> Driver::Status
        {
            active();
            const auto state = m_driver->status();
            require(state, "UART status");
            require(!state->reset() && !state->fault() && !state->warning(),
                    "driver reset/fault/thermal warning");
            require(bool(state->input & 1U) != enabled, "driver ENN readback disagrees with enable GPIO");
            require(m_driver->verify(), "configuration readback");
            active();
            return *state;
        }

        static void printStatus(const Driver::Status& s)
        {
            log("GSTAT=%08lx DRV_STATUS=%08lx IOIN=%08lx SG_RESULT=%u open-load=%u standstill=%u chopper=%s "
                "CS_ACTUAL=%u",
                static_cast<unsigned long>(s.global),
                static_cast<unsigned long>(s.driver),
                static_cast<unsigned long>(s.input),
                unsigned(s.load),
                unsigned(s.openLoad()),
                unsigned(s.standstill()),
                modeName((s.driver & (1U << 30U)) ? Driver::Mode::StealthChop : Driver::Mode::SpreadCycle),
                unsigned((s.driver >> 16U) & 31U));
        }

        auto readRegister(std::uint8_t reg) -> std::uint32_t
        {
            std::array<std::uint8_t, 4> request{ 5, m_address, reg, 0 };
            request.back() = Driver::crc(std::span{ request }.first(3));
            std::array<std::uint8_t, 8> reply{};
            require(m_bus->exchange(request, reply, 10ms), "read driver register");
            require(reply[0] == 5 && reply[1] == 0xFF && reply[2] == reg &&
                      Driver::crc(std::span{ reply }.first(7)) == reply.back(),
                    "register reply CRC/header");
            return (std::uint32_t{ reply[3] } << 24U) | (std::uint32_t{ reply[4] } << 16U) |
                   (std::uint32_t{ reply[5] } << 8U) | reply[6];
        }

        auto readCounter() -> std::uint8_t { return static_cast<std::uint8_t>(readRegister(0x02)); }

        void check()
        {
            m_ready.store(false);
            active();
            static_cast<void>(m_generator->stop());
            require(m_step->clear(), "clear previous pulse schedule");
            require(m_generator->start(), "restart idle timebase");
            const std::array<std::uint8_t, 1> addresses{ m_address };
            require(Driver::prepareBus(*m_bus, addresses), "prepare single-node UART");
            require(m_driver->initialize(m_configuration), "initialize selected driver");
            printStatus(sample(false));
            log("Identity, address straps, ENN, current writes/IFCNT and configuration verified.");

            // A bad-CRC VACTUAL=0 write is harmless even if incorrectly accepted.
            // IFCNT must not increment; this tests the chip's receiver, not just ours.
            const auto counter = readCounter();
            std::array<std::uint8_t, 8> invalid{ 5, m_address, 0xA2, 0, 0, 0, 0, 0 };
            invalid.back() = Driver::crc(std::span{ invalid }.first(7)) ^ 1U;
            require(m_bus->exchange(invalid, {}, 10ms), "bad-CRC frame echo");
            require(readCounter() == counter, "driver accepted a bad-CRC write");
            log("Bad-CRC write rejected (IFCNT unchanged).");

            Driver absent{ m_bus, static_cast<std::uint8_t>(m_address ^ 1U) };
            auto missing = absent.status();
            require(!missing && missing.error() == hal::make_error_code(hal::HalError::Timeout),
                    "unused address must time out");
            printStatus(sample(false)); // prove recovery after the deliberate timeout
            log("Unused address timeout and subsequent valid transaction verified.");

            auto alternate = m_configuration;
            alternate.hold_milliamps = 400;
            alternate.microsteps = 32;
            alternate.mode = m_configuration.mode == Driver::Mode::StealthChop ? Driver::Mode::SpreadCycle
                                                                               : Driver::Mode::StealthChop;
            alternate.interpolate = false;
            require(m_driver->initialize(alternate), "alternate configuration while disabled");
            static_cast<void>(sample(false));
            require(m_driver->initialize(m_configuration), "restore selected comparison settings");
            log("Disabled configuration round trip: current, mode, microsteps and interpolation.");
            for (unsigned n{}; n < 100; ++n) {
                static_cast<void>(sample(false));
                if ((n + 1) % 25 == 0)
                    log("Repeated status/configuration reads: %u/100", n + 1);
            }
            require((GPIOA->IDR & GPIO_PIN_0) == 0 && (GPIOB->IDR & (GPIO_PIN_10 | GPIO_PIN_11)) == 0,
                    "all STEP pads must remain low");
            {
                const hal::stm32::InterruptGuard guard;
                if (!m_abort.load() && !m_fault.load() && m_diag->read() == Level::Low &&
                    !m_button->isPressed())
                    m_ready.store(true);
            }
            active();
            require(m_ready.load(), "check cancelled before qualification");
        }

        void enable(const Driver::Configuration& configuration)
        {
            require(m_ready.load(), "run check before enabling");
            require(m_driver->configuration() == configuration, "unexpected motion configuration");
            static_cast<void>(sample(false));
            bool enabled{};
            {
                // Do not overwrite a concurrent stop/DIAG interrupt with EN low.
                const hal::stm32::InterruptGuard guard;
                if (!m_abort.load() && !m_fault.load() && m_diag->read() == Level::Low &&
                    !m_button->isPressed()) {
                    m_enable->write(Level::Low);
                    enabled = true;
                }
            }
            require(enabled, "enable cancelled");
            // Allow StealthChop's standstill auto-tuning, identically in both modes.
            dwell(1s, true);
            const auto state = sample(true);
            require(bool(state.driver & (1U << 30U)) == (configuration.mode == Driver::Mode::StealthChop),
                    "driver reports wrong active chopper mode");
            const auto scale = Driver::currentScale(configuration.run_milliamps);
            require(scale, "comparison current scale");
            require(((state.driver >> 16U) & 31U) == *scale, "driver reports wrong current scale");
            printStatus(state);
        }

        void dwell(std::chrono::milliseconds duration, bool enabled)
        {
            const auto end = Clock::now() + duration;
            while (Clock::now() < end) {
                static_cast<void>(sample(enabled));
                require((GPIOB->IDR & GPIO_PIN_10) == 0, "STEP must stay low during dwell/hold");
                m_notification.waitUntil(std::min(end, Clock::now() + 20ms));
            }
            active();
        }

        void hold()
        {
            const auto phase = readRegister(0x6A); // MSCNT: commanded electrical phase, not shaft feedback.
            const auto counter = readCounter();
            log("HOLD %s for 5 s; compare buzzing/vibration and holding strength.",
                modeName(m_configuration.mode));
            dwell(5s, true);
            const auto state = sample(true);
            printStatus(state);
            require(state.standstill(), "driver must report standstill during hold");
            require(readRegister(0x6A) == phase, "microstep counter changed during hold");
            require(readCounter() == counter, "unexpected register write during hold");
            log("HOLD verified: STEP low, MSCNT=%lu unchanged, UART reads only.",
                static_cast<unsigned long>(phase));
        }

        void leg(unsigned index,
                 Level direction,
                 hal::step::PulseCount pulses = 400,
                 std::chrono::microseconds period = 2500us)
        {
            active();
            m_dir->write(direction);
            require(m_step->prepare({ period, 5us }, pulses), "prepare finite move");
            require(m_step->start(2ms), "start finite move");
            const auto deadline = Clock::now() + period * static_cast<std::int64_t>(pulses) + 1s;
            for (;;) {
                active();
                const auto state = m_step->status();
                if (state.state == hal::step::State::Completed) {
                    require(state.counts_exact && state.pulses == pulses, "finite pulse count");
                    hardware_tmc_test_pulses[index] = state.pulses;
                    require((GPIOB->IDR & GPIO_PIN_10) == 0, "M2 STEP stopped low");
                    static_cast<void>(sample(true));
                    log("LEG %u: DIR=%u pulses=%llu exact=%u STEP=low",
                        index + 1,
                        direction == Level::High ? 1U : 0U,
                        static_cast<unsigned long long>(state.pulses),
                        unsigned(state.counts_exact));
                    return;
                }
                require(state.state == hal::step::State::Running, "step generator stopped/faulted");
                require(Clock::now() < deadline, "motion deadline exceeded");
                static_cast<void>(sample(true));
                m_notification.waitUntil(std::min(deadline, Clock::now() + 20ms));
            }
        }

        void indexLeg(unsigned index, Level direction, std::chrono::microseconds period)
        {
            constexpr hal::step::PulseCount pulses = 129; // Odd count distinguishes edges from full cycles.
            const auto before = readRegister(0x6A);
            const auto writes = readCounter();
            log("INDEX LEG %u: DIR=%u pulses=129 period=%lld us high=5 us MSCNT_before=%lu",
                index + 1,
                direction == Level::High ? 1U : 0U,
                static_cast<long long>(period.count()),
                static_cast<unsigned long>(before));
            leg(index, direction, pulses, period);
            dwell(200ms, true); // Let interpolation finish before checking the final electrical phase.
            const auto after = readRegister(0x6A);
            const auto delta =
              static_cast<std::uint32_t>(pulses * (256U / m_configuration.microsteps)) & 1023U;
            const auto expected = (before + (direction == Level::Low ? delta : 1024U - delta)) & 1023U;
            log("INDEX LEG %u: MSCNT_after=%lu expected=%lu",
                index + 1,
                static_cast<unsigned long>(after),
                static_cast<unsigned long>(expected));
            require(after == expected, "external STEP/DIR did not produce the expected MSCNT change");
            require(readCounter() == writes, "unexpected register write during INDEX leg");
        }

        void indexTest()
        {
            require(m_configuration.mode == Driver::Mode::StealthChop, "INDEX test requires StealthChop");
            log("INDEX: capture D0 STEP, D1 DIR, D2 EN and D7 INDEX for 25 s starting now.");
            log("Three phases; each moves 129 pulses out at 100 Hz, back at 400 Hz. VACTUAL stays zero.");
            log("Enabling in 2 seconds; stop/B1 aborts.");
            dwell(2s, false);
            for (unsigned phase{}; phase < 3; ++phase) {
                active();
                quiesce();
                auto configuration = m_configuration;
                configuration.index_step = phase != 0;
                configuration.interpolate = phase != 2;
                require(m_driver->initialize(configuration), "configure INDEX test phase while disabled");
                static_cast<void>(sample(false));
                log("INDEX PHASE %u/3: index_step=%u interpolation=%u GCONF=%08lx CHOPCONF=%08lx",
                    phase + 1,
                    unsigned(configuration.index_step),
                    unsigned(configuration.interpolate),
                    static_cast<unsigned long>(readRegister(0x00)),
                    static_cast<unsigned long>(readRegister(0x6C)));
                enable(configuration);
                const auto origin = readRegister(0x6A);
                indexLeg(0, Level::Low, 10ms);
                dwell(300ms, true);
                indexLeg(1, Level::High, 2500us);
                require(readRegister(0x6A) == origin, "INDEX phase failed to return to its initial MSCNT");
                quiesce();
                dwell(500ms, false); // Disabled gap separates phases in the capture.
            }
            // run() restores the selected configuration on success AND failure.
            log("INDEX sequence complete. D7 is analyzer-only: inspect capture to determine its behavior.");
        }

        void feedbackTest()
        {
            hal::device::IndexFeedback feedback{ hal::board::createStepperIndex(motor) };
            std::atomic<std::int64_t> cycles{};
            std::atomic<double> velocity{}, peak_forward{}, peak_reverse{};
            std::atomic_bool valid{};
            feedback.setCallback([&](const auto& value) noexcept {
                valid.store(bool(value.cycles));
                if (value.cycles) cycles.store(*value.cycles);
                velocity.store(value.cycles_per_second);
                if (value.cycles_per_second > peak_forward.load()) peak_forward.store(value.cycles_per_second);
                if (value.cycles_per_second < peak_reverse.load()) peak_reverse.store(value.cycles_per_second);
            });
            struct Cleanup
            {
                Bench& bench;
                hal::device::IndexFeedback& feedback;
                ~Cleanup()
                {
                    bench.quiesce();
                    static_cast<void>(bench.m_step->setProgressCallback({}));
                    feedback.clearCallback();
                }
            } cleanup{ *this, feedback };
            require(m_step->setProgressCallback([&](const hal::step::AxisStatus& state) noexcept {
                feedback.motion(state.state == hal::step::State::Running, m_dir->read() == Level::High,
                                state.period * 64);
            }), "INDEX progress subscription");
            log("FEEDBACK: normal INDEX, PD0/CN9.25, 64 STEP per cycle, 7.2 deg per cycle.");
            log("Enabling in 2 seconds; stop/B1 aborts.");
            dwell(2s, false);
            enable(m_configuration);
            leg(0, Level::High, 128, 2500us); // Establish phase before comparing closed excursions.
            dwell(200ms, true);
            require(valid.load(), "no INDEX callback: check PD0/CN9.25 wiring");
            const auto origin{ cycles.load() };
            const auto phase{ readRegister(0x6A) };
            peak_forward.store(0);
            peak_reverse.store(0);
            leg(0, Level::High, 256, 10ms);
            dwell(200ms, true);
            log("FEEDBACK forward: cycles=%lld expected=%lld peak=%f cycles/s stopped=%f",
                static_cast<long long>(cycles.load()), static_cast<long long>(origin + 4),
                peak_forward.load(), velocity.load());
            require(valid.load() && cycles.load() == origin + 4 && velocity.load() == 0,
                    "forward INDEX cycles/stop velocity");
            leg(1, Level::Low, 256, 2500us);
            dwell(200ms, true);
            log("FEEDBACK reverse: cycles=%lld expected=%lld peak=%f cycles/s stopped=%f",
                static_cast<long long>(cycles.load()), static_cast<long long>(origin),
                peak_reverse.load(), velocity.load());
            require(valid.load() && cycles.load() == origin && velocity.load() == 0,
                    "reverse INDEX cycles/stop velocity");
            require(peak_forward.load() > 1.5 && peak_forward.load() < 1.65 &&
                    peak_reverse.load() < -6.1 && peak_reverse.load() > -6.4,
                    "INDEX velocity at 100/400 Hz STEP");
            // Cross and recross a boundary with short moves as well.
            leg(0, Level::High, 16, 10ms);
            dwell(200ms, true);
            leg(1, Level::Low, 16, 10ms);
            dwell(200ms, true);
            require(valid.load() && cycles.load() == origin && velocity.load() == 0,
                    "short reversal produced INDEX count drift");
            require(readRegister(0x6A) == phase, "feedback test final electrical phase");
            log("FEEDBACK verified: forward, reverse, short reversal, measured speeds, stopped velocity.");
        }

        void run(int operation) noexcept
        {
            const auto name = std::array{ "check", "status", "hold", "move", "index", "feedback" }[operation];
            hardware_tmc_test_status = 0x52554E00U;
            bool passed{};
            try {
                quiesce();
                // Only command("check") acknowledges an old abort/fault,
                // before launching this worker. A newer stop is never cleared.
                log("BEGIN %s address=%u; EN disabled", name, unsigned(m_address));
                log("Selected %s; run=hold=%u mA requested; %u microsteps; qualified=%u",
                    modeName(m_configuration.mode),
                    unsigned(m_configuration.run_milliamps),
                    unsigned(m_configuration.microsteps),
                    unsigned(m_ready.load()));
                if (operation == 0)
                    check();
                else if (operation == 1) {
                    const auto state = m_driver->status();
                    require(state, "read raw diagnostics");
                    printStatus(*state);
                }
                else {
                    hardware_tmc_test_pulses[0] = 0;
                    hardware_tmc_test_pulses[1] = 0;
                    if (operation == 4)
                        indexTest();
                    else if (operation == 5)
                        feedbackTest();
                    else {
                        log("Enabling in 2 seconds; stop/B1 aborts.");
                        dwell(2s, false);
                        enable(m_configuration);
                        if (operation == 2)
                            hold();
                        else {
                            leg(0, Level::Low);
                            dwell(300ms, true);
                            leg(1, Level::High);
                        }
                    }
                    quiesce();
                    printStatus(sample(false));
                }
                passed = true;
            } catch (const std::exception& error) {
                quiesce();
                m_ready.store(false);
                log("FAIL %s: %s", name, error.what());
            } catch (...) {
                quiesce();
                m_ready.store(false);
                log("FAIL %s: unexpected exception", name);
            }
            quiesce();
            if (operation == 4) {
                // No enable, STEP or motion retry during cleanup. A lost UART
                // leaves qualification false until a complete check succeeds.
                try {
                    require(m_driver->initialize(m_configuration), "restore normal INDEX configuration");
                    log("INDEX restored: index_step=0, interpolation=1; EN disabled.");
                } catch (const std::exception& error) {
                    passed = false;
                    m_ready.store(false);
                    log("FAIL INDEX restore: %s; EN disabled; run check.", error.what());
                } catch (...) {
                    passed = false;
                    m_ready.store(false);
                    log("FAIL INDEX restore: unexpected exception; EN disabled; run check.");
                }
                if (passed && (m_abort.load() || m_fault.load())) {
                    passed = false;
                    m_ready.store(false);
                    log("FAIL index: stopped during cleanup; EN disabled; run check.");
                }
            }
            hardware_tmc_test_status = passed ? 0x600D600DU : 0xBAD00000U | operation;
            hardware_tmc_test_complete();
            if (passed)
                log("PASS %s; EN disabled", name);
            m_busy.store(false);
            log("READY for command");
        }

        std::shared_ptr<hal::IDigitalOutput> m_enable, m_dir;
        std::shared_ptr<hal::IStepGenerator> m_generator;
        std::shared_ptr<hal::IStepOutput> m_step;
        std::shared_ptr<hal::IDigitalInput> m_diag;
        std::shared_ptr<hal::device::IButton> m_button;
        std::shared_ptr<hal::IUart> m_bus;
        std::unique_ptr<Driver> m_driver;
        Driver::Configuration m_configuration{ benchConfig };
        std::uint8_t m_address{};
        std::atomic_bool m_abort{}, m_fault{}, m_ready{}, m_busy{};
        runtime::Notification m_notification;
        std::jthread m_worker;
    };
}

int main()
{
    try {
        Bench bench;
        log("READY: single TMC2209 bench; address=0; EN disabled; no startup motion");
        Bench::menu();
        std::array<char, 80> line{};
        std::size_t size{};
        bool overflow{};
        for (;;) {
            const auto c = std::getchar();
            require(c != EOF, "console input failed");
            if (c == '\0')
                continue; // Ignore ST-Link line-coding-change noise.
            if (c == '\n') {
                if (overflow)
                    log("REJECTED: line too long");
                else
                    bench.command(std::string_view{ line.data(), size });
                size = 0;
                overflow = false;
            }
            else if (!overflow && (c == '\b' || c == 127)) {
                if (size != 0)
                    --size;
            }
            else if (!overflow) {
                if (size == line.size())
                    overflow = true;
                else
                    line[size++] = static_cast<char>(c);
            }
        }
    } catch (const std::exception& error) {
        log("FATAL: %s", error.what());
        return 1;
    }
}
