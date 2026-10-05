#include "hal/board/board.hpp"
#include "hal/devices/impl/Tmc2209.hpp"
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
#include <cstring>
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
    constexpr Driver::Configuration benchConfig{ .run_milliamps = 400, .hold_milliamps = 400 };
    static_assert(std::atomic_bool::is_always_lock_free);

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
            if (command == "address 0" || command == "address 1") {
                quiesce();
                m_ready.store(false);
                m_address = command.back() - '0';
                m_driver = std::make_unique<Driver>(m_bus, m_address);
                log("Selected address %u; STEP/DIR/DIAG remain on M2 pins. Run check.", unsigned(m_address));
                return;
            }
            if (command != "check" && command != "status" && command != "hold" && command != "move") {
                menu();
                return;
            }
            if ((command == "hold" || command == "move") && !m_ready.load()) {
                log("REJECTED: check must pass before enabling the motor.");
                return;
            }
            // The selected operation outlives the input buffer; capture a value.
            const auto operation = command == "check"    ? 0
                                   : command == "status" ? 1
                                   : command == "hold"   ? 2
                                                         : 3;
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
            log("hold = energize 2 s without STEP; move = +400 then -400 pulses, 400 Hz / 5 us high");
            log("stop = disable/abort; address 0|1 = select strapped node; help = this menu");
            log("Only ONE TMC2209. Physical M2 pins for either address; no robot/perfboard required.");
            log("400 mA RMS requested (nominal 397 mA), 16 microsteps, SpreadCycle during motion.");
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
            log("GSTAT=%08lx DRV_STATUS=%08lx IOIN=%08lx SG_RESULT=%u open-load=%u standstill=%u",
                static_cast<unsigned long>(s.global),
                static_cast<unsigned long>(s.driver),
                static_cast<unsigned long>(s.input),
                unsigned(s.load),
                unsigned(s.openLoad()),
                unsigned(s.standstill()));
        }

        auto readCounter() -> std::uint8_t
        {
            std::array<std::uint8_t, 4> request{ 5, m_address, 2, 0 };
            request.back() = Driver::crc(std::span{ request }.first(3));
            std::array<std::uint8_t, 8> reply{};
            require(m_bus->exchange(request, reply, 10ms), "read IFCNT");
            require(reply[0] == 5 && reply[1] == 0xFF && reply[2] == 2 &&
                      Driver::crc(std::span{ reply }.first(7)) == reply.back(),
                    "IFCNT reply CRC/header");
            return reply[6];
        }

        void check()
        {
            m_ready.store(false);
            active();
            static_cast<void>(m_generator->stop());
            require(m_step->clear(), "clear previous pulse schedule");
            require(m_generator->start(), "restart idle timebase");
            const std::array<std::uint8_t, 1> addresses{ m_address };
            require(Driver::prepareBus(*m_bus, addresses), "prepare single-node UART");
            require(m_driver->initialize(benchConfig), "initialize selected driver");
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

            auto alternate = benchConfig;
            alternate.run_milliamps = alternate.hold_milliamps = 550;
            alternate.microsteps = 32;
            alternate.mode = Driver::Mode::StealthChop;
            alternate.interpolate = false;
            require(m_driver->initialize(alternate), "alternate configuration while disabled");
            static_cast<void>(sample(false));
            require(m_driver->initialize(benchConfig), "restore conservative motion settings");
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

        void enable()
        {
            require(m_ready.load(), "run check before enabling");
            require(m_driver->configuration() == benchConfig, "unexpected motion configuration");
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
            dwell(200ms, true);
        }

        void dwell(std::chrono::milliseconds duration, bool enabled)
        {
            const auto end = Clock::now() + duration;
            while (Clock::now() < end) {
                static_cast<void>(sample(enabled));
                m_notification.waitUntil(std::min(end, Clock::now() + 20ms));
            }
            active();
        }

        void leg(unsigned index, Level direction)
        {
            active();
            m_dir->write(direction);
            require(m_step->prepare({ 2500us, 5us }, 400), "prepare 400-pulse move");
            require(m_step->start(2ms), "start finite move");
            const auto deadline = Clock::now() + 2s;
            for (;;) {
                active();
                const auto state = m_step->status();
                if (state.state == hal::step::State::Completed) {
                    require(state.counts_exact && state.pulses == 400, "finite pulse count");
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

        void run(int operation) noexcept
        {
            const auto name = std::array{ "check", "status", "hold", "move" }[operation];
            hardware_tmc_test_status = 0x52554E00U;
            bool passed{};
            try {
                quiesce();
                // Only command("check") acknowledges an old abort/fault,
                // before launching this worker. A newer stop is never cleared.
                log("BEGIN %s address=%u; EN disabled", name, unsigned(m_address));
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
                    log("Enabling in 2 seconds; stop/B1 aborts.");
                    dwell(2s, false);
                    enable();
                    if (operation == 2)
                        dwell(2s, true);
                    else {
                        leg(0, Level::Low);
                        dwell(300ms, true);
                        leg(1, Level::High);
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
        while (true) {
            std::array<char, 80> line{};
            if (!std::fgets(line.data(), line.size(), stdin)) {
                log("FATAL: console input failed");
                return 1;
            }
            const auto newline = std::strchr(line.data(), '\n');
            if (!newline) {
                // Reject an overlong line as a unit; never execute its suffix.
                int c;
                do {
                    c = std::getchar();
                } while (c != '\n' && c != EOF);
                log("REJECTED: line too long");
                continue;
            }
            *newline = '\0';
            bench.command(line.data());
        }
    } catch (const std::exception& error) {
        log("FATAL: %s", error.what());
        return 1;
    }
}
