#include "hal/board/board.hpp"
#include "hal/hal.hpp"
#include "hal/stm32/InterruptGuard.hpp"
#include "runtime/synchronization/Notification.hpp"
#include "runtime/thread.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

// No Tmc2209/IUart object or driver register access belongs in this image.
// Break only after a case finishes, with the output disabled.
extern "C" {
volatile std::uint32_t hardware_tmc_standalone_test_status{};
volatile std::uint32_t hardware_tmc_standalone_test_pulses[2]{};
[[gnu::noinline, gnu::used]] void hardware_tmc_standalone_test_complete() { asm volatile("" ::: "memory"); }
}

namespace
{
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    using hal::gpio::Level;
    constexpr auto motor = hal::board::MotorId::Motor2;
    constexpr std::uint64_t pulseCount = 400;
    // MS1=MS2=GND selects 1/8 microsteps after a driver power cycle:
    // 400 pulses = 90 degrees for a 1.8-degree motor; 200 Hz = 7.5 RPM.
    constexpr auto period = 5ms;
    constexpr auto highTime = 5us;
    static_assert(std::atomic_bool::is_always_lock_free);

    template<typename... Args>
    void log(const char* format, Args... args)
    {
        std::printf("[tmc-standalone] ");
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

    enum class Operation
    {
        Check,
        Hold,
        Move,
        Forward,
        Reverse
    };

    class Bench final
    {
      public:
        Bench()
        {
            m_enable = hal::board::createSteppersEnableOutput();
            require(bool(m_enable), "enable output unavailable");
            m_enable->write(Level::High);
            m_generator = hal::board::createStepperGenerator();
            m_step = hal::board::createStepperStepOutput(m_generator, motor);
            m_dir = hal::board::createStepperDirectionOutput(motor);
            m_diag = hal::board::createStepperDiagnostic(motor);
            m_button = hal::board::createButton(hal::board::ButtonId::User);
            require(m_generator && m_step && m_dir && m_diag && m_button, "bench resources unavailable");
            require(m_step->setCompletionCallback(
                      [this](const hal::step::AxisStatus&) noexcept { m_notification.signal(); }),
                    "completion callback");
            require(m_generator->start(), "start idle timebase");
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
            // B1/DIAG may call this from an ISR. Disable the bridge immediately;
            // the worker wakes and stops the pulse schedule in thread context.
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
            if (command == "status") {
                log("EN=%s DIAG=%u busy=%u ready=%u abort=%u fault=%u; driver UART unused",
                    m_enable->read() == Level::High ? "disabled" : "enabled",
                    m_diag->read() == Level::High ? 1U : 0U,
                    unsigned(m_busy.load()),
                    unsigned(m_ready.load()),
                    unsigned(m_abort.load()),
                    unsigned(m_fault.load()));
                return;
            }
            if (command.empty() || command == "help") {
                menu();
                return;
            }
            if (m_busy.load()) {
                log("BUSY; stop or B1 aborts; status reports GPIO levels.");
                return;
            }
            if (m_worker.joinable())
                m_worker.join();
            Operation operation{};
            if (command == "check")
                operation = Operation::Check;
            else if (command == "hold")
                operation = Operation::Hold;
            else if (command == "move")
                operation = Operation::Move;
            else if (command == "forward")
                operation = Operation::Forward;
            else if (command == "reverse")
                operation = Operation::Reverse;
            else {
                log("Unknown command; type help.");
                return;
            }
            if (operation == Operation::Check) {
                // Acknowledge only an old stop/fault, before the worker starts.
                // Never clear a newer stop from inside the worker.
                const hal::stm32::InterruptGuard guard;
                m_ready.store(false);
                m_abort.store(false);
                m_fault.store(false);
            }
            else if (!m_ready.load()) {
                log("REJECTED: run check first. Physically set current and power-cycle the driver.");
                return;
            }
            m_busy.store(true);
            try {
                m_worker = runtime::thread::create_jthread({ .name = "TMC standalone", .stack_size = 16384 },
                                                           [this, operation] { run(operation); });
            } catch (...) {
                quiesce();
                m_ready.store(false);
                m_busy.store(false);
                throw;
            }
        }

        static void menu()
        {
            log("check = GPIO/timebase check only; NEVER reads or writes the driver UART");
            log("hold = energize 5 s; move = 400 pulses forward, pause, 400 pulses reverse");
            log("forward / reverse = one 400-pulse leg; all moves use 200 Hz / 5 us high");
            log("stop / B1 = disable and abort; status = GPIO state; help = this menu");
            log("MS1=MS2=GND: 1/8 microsteps, each leg 90 degrees in 2 s (1.8-degree motor).");
            log("Current comes from the POTENTIOMETER. Power-cycle the driver after UART testing.");
            log("No startup motion. Each powered test waits 2 s before enabling.");
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

        void check()
        {
            active();
            require((RCC->APB1LENR & RCC_APB1LENR_USART2EN) == 0, "driver USART2 must remain clock-disabled");
            constexpr std::uint32_t pinModes = (3U << (5U * 2U)) | (3U << (6U * 2U));
            require((GPIOD->MODER & pinModes) == pinModes && (GPIOD->PUPDR & pinModes) == 0,
                    "PD5/PD6 must remain analog/no-pull (high impedance)");
            static_cast<void>(m_generator->stop());
            require(m_step->clear(), "clear previous schedule");
            require(m_generator->start(), "restart idle timebase");
            require((GPIOA->IDR & GPIO_PIN_0) == 0 && (GPIOB->IDR & (GPIO_PIN_10 | GPIO_PIN_11)) == 0,
                    "all STEP pads must be low");
            {
                const hal::stm32::InterruptGuard guard;
                if (!m_abort.load() && !m_fault.load() && m_diag->read() == Level::Low &&
                    !m_button->isPressed())
                    m_ready.store(true);
            }
            active();
            require(m_ready.load(), "check cancelled");
            log("GPIO/timebase OK; USART2 off, PD5/PD6 high impedance. Driver current/configuration NOT "
                "verified.");
        }

        void dwell(std::chrono::milliseconds duration)
        {
            const auto end = Clock::now() + duration;
            while (Clock::now() < end) {
                active();
                m_notification.waitUntil(end);
            }
            active();
        }

        void enable()
        {
            bool enabled{};
            {
                const hal::stm32::InterruptGuard guard;
                if (m_ready.load() && !m_abort.load() && !m_fault.load() && m_diag->read() == Level::Low &&
                    !m_button->isPressed()) {
                    m_enable->write(Level::Low);
                    enabled = true;
                }
            }
            require(enabled, "enable cancelled");
            log("ENABLED; stop or B1 disables immediately.");
        }

        void leg(unsigned index, Level direction)
        {
            active();
            m_dir->write(direction);
            require(m_step->prepare({ period, highTime }, pulseCount), "prepare finite move");
            require(m_step->start(2ms), "start finite move");
            const auto deadline = Clock::now() + 3s;
            for (;;) {
                active();
                const auto state = m_step->status();
                if (state.state == hal::step::State::Completed) {
                    require(state.counts_exact && state.pulses == pulseCount, "finite pulse count");
                    require((GPIOB->IDR & GPIO_PIN_10) == 0, "STEP must stop low");
                    hardware_tmc_standalone_test_pulses[index] = state.pulses;
                    log("LEG %u: DIR=%u pulses=%llu exact=%u STEP=low",
                        index + 1,
                        direction == Level::High ? 1U : 0U,
                        static_cast<unsigned long long>(state.pulses),
                        unsigned(state.counts_exact));
                    return;
                }
                require(state.state == hal::step::State::Running, "step generator stopped/faulted");
                require(Clock::now() < deadline, "motion deadline exceeded");
                m_notification.waitUntil(deadline);
            }
        }

        void run(Operation operation) noexcept
        {
            const auto name =
              std::array{ "check", "hold", "move", "forward", "reverse" }[unsigned(operation)];
            hardware_tmc_standalone_test_status = 0x52554E00U;
            hardware_tmc_standalone_test_pulses[0] = 0;
            hardware_tmc_standalone_test_pulses[1] = 0;
            bool passed{};
            try {
                quiesce();
                log("BEGIN %s; no driver UART; EN disabled", name);
                if (operation == Operation::Check)
                    check();
                else {
                    log("Enabling in 2 seconds; current is set by the potentiometer.");
                    dwell(2s);
                    enable();
                    if (operation == Operation::Hold)
                        dwell(5s);
                    else {
                        dwell(1s); // Standstill settling / default StealthChop automatic tuning.
                        leg(0, operation == Operation::Reverse ? Level::High : Level::Low);
                        if (operation == Operation::Move) {
                            dwell(500ms);
                            leg(1, Level::High);
                        }
                    }
                    active();
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
            hardware_tmc_standalone_test_status = passed ? 0x600D600DU : 0xBAD00000U | unsigned(operation);
            hardware_tmc_standalone_test_complete();
            if (passed)
                log("PASS %s; EN disabled. This does not verify shaft movement or winding current.", name);
            m_busy.store(false);
            log("READY for command");
        }

        std::shared_ptr<hal::IDigitalOutput> m_enable, m_dir;
        std::shared_ptr<hal::IStepGenerator> m_generator;
        std::shared_ptr<hal::IStepOutput> m_step;
        std::shared_ptr<hal::IDigitalInput> m_diag;
        std::shared_ptr<hal::device::IButton> m_button;
        std::atomic_bool m_abort{}, m_fault{}, m_ready{}, m_busy{};
        runtime::Notification m_notification;
        std::jthread m_worker;
    };
}

int main()
{
    try {
        Bench bench;
        log("READY: STEP/DIR standalone bench; USART2 off; EN disabled; no startup motion");
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
