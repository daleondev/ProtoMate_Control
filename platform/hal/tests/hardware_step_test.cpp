#include "hal/board/board.hpp"
#include "hal/hal.hpp"
#include "hal/stm32/InterruptGuard.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <thread>
#include <vector>

extern "C" {
// Inspect after hardware_step_test_complete(), never halt during a capture.
volatile std::uint32_t hardware_step_test_case{};
volatile std::uint32_t hardware_step_test_status{};
volatile std::uint64_t hardware_step_test_pulses[3]{};
volatile std::uint32_t hardware_step_test_guard_stopped{};
[[gnu::noinline, gnu::used]] void hardware_step_test_complete() { asm volatile("" ::: "memory"); }
}

namespace
{
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    using Counts = std::array<hal::step::PulseCount, 3>;
    using hal::step::State;
    constexpr std::array dma_irqs{ DMA1_Stream0_IRQn,
                                   DMA1_Stream1_IRQn,
                                   DMA1_Stream2_IRQn,
                                   DMA1_Stream3_IRQn };

    template<typename... Args>
    void log(const char* format, Args... args)
    {
        std::printf("[step-test] ");
        std::printf(format, args...);
        std::printf("\n");
        std::fflush(stdout);
    }

    auto stateName(State state) -> const char*
    {
        switch (state) {
            case State::Idle:
                return "Idle";
            case State::Ready:
                return "Ready";
            case State::Running:
                return "Running";
            case State::Completed:
                return "Completed";
            case State::Stopped:
                return "Stopped";
            case State::Underrun:
                return "Underrun";
            case State::DmaError:
                return "DmaError";
        }
        return "unknown";
    }

    auto padsLow() -> bool
    {
        return (GPIOA->IDR & GPIO_PIN_0) == 0U && (GPIOB->IDR & (GPIO_PIN_10 | GPIO_PIN_11)) == 0U;
    }

    // Mask only the four CPU interrupt vectors. DMA and TIM2 keep running;
    // TIM5, the HAL tick, ThreadX and UART interrupts remain available.
    class WithholdDmaInterrupts final
    {
      public:
        WithholdDmaInterrupts()
        {
            for (std::size_t i = 0; i < dma_irqs.size(); ++i) {
                m_enabled[i] = NVIC_GetEnableIRQ(dma_irqs[i]) != 0U;
                HAL_NVIC_DisableIRQ(dma_irqs[i]);
            }
            __DSB();
            __ISB();
        }
        ~WithholdDmaInterrupts()
        {
            // Do not clear pending flags: restoration must service real progress.
            for (std::size_t i = 0; i < dma_irqs.size(); ++i) {
                if (m_enabled[i]) {
                    HAL_NVIC_EnableIRQ(dma_irqs[i]);
                }
            }
        }

      private:
        std::array<bool, 4> m_enabled{};
    };

    void busyWait(std::chrono::microseconds duration)
    {
        const auto until{ Clock::now() + duration };
        while (Clock::now() < until) {
            __NOP();
        }
    }

    class Bench final
    {
      public:
        ~Bench()
        {
            if (m_generator) {
                static_cast<void>(m_generator->stop());
                static_cast<void>(m_generator->setProgressCallback({}));
            }
        }
        auto initialize() -> bool
        {
            m_enable = hal::board::createSteppersEnableOutput();
            m_generator = hal::board::createStepperGenerator();
            m_button = hal::board::createButton(hal::board::ButtonId::User);
            m_green = hal::board::createLed(hal::board::LedId::Green);
            m_red = hal::board::createLed(hal::board::LedId::Red);
            m_yellow = hal::board::createLed(hal::board::LedId::Yellow);
            if (!m_enable || !m_generator || !m_button || !m_green || !m_red || !m_yellow) {
                return false;
            }
            for (std::size_t i = 0; i < 3U; ++i) {
                const auto motor{ static_cast<hal::board::MotorId>(i) };
                m_axes[i] = hal::board::createStepperStepOutput(m_generator, motor);
                m_directions[i] = hal::board::createStepperDirectionOutput(motor);
                if (!m_axes[i] || !m_directions[i]) {
                    return false;
                }
            }
            return m_generator
                     ->setProgressCallback([this](const hal::step::Status& status) noexcept {
                // No allocation, logging, RTOS calls or mutation from the ISR.
                for (std::size_t i = 0; i < 3U; ++i) {
                    m_progress.monotonic &= status.pulses[i] >= m_progress.last.pulses[i];
                }
                ++m_progress.calls;
                m_progress.last = status;
            }).has_value() &&
                   idle();
        }

        auto run(unsigned test) -> bool
        {
            hardware_step_test_case = test;
            hardware_step_test_status = 0x52554E00U;
            hardware_step_test_guard_stopped = 0U;
            m_green->turnOff();
            m_red->turnOff();
            m_yellow->turnOn();
            static_cast<void>(m_generator->stop());
            for (auto& axis : m_axes) {
                if (!axis->clear()) {
                    return report(false);
                }
            }
            {
                const hal::stm32::InterruptGuard lock;
                m_progress = {};
            }
            log("CASE %u: arm the analyzer; starting in 2 seconds; B1 aborts", test);
            std::this_thread::sleep_for(2s);
            if (!check(idle(), "initial STEP/enable/DIR levels") ||
                !check(!m_button->isPressed(), "release B1 before starting")) {
                return report(false);
            }
            bool passed{};
            switch (test) {
                case 1:
                    passed = constant({ 1000us, 1000us, 1000us }, Counts{ 1U, 2U, 3U }) &&
                             startAndWait(100ms) && final(State::Completed, Counts{ 1U, 2U, 3U });
                    break;
                case 2:
                    passed = constant({ 1000us, 500us, 250us }, Counts{ 1000U, 1500U, 2000U }) &&
                             startAndWait(2s) && final(State::Completed, Counts{ 1000U, 1500U, 2000U });
                    break;
                case 3:
                    passed = constant({ 10us, 10us, 10us }, Counts{ 100000U, 100000U, 100000U }) &&
                             startAndWait(2s) && final(State::Completed, Counts{ 100000U, 100000U, 100000U });
                    break;
                case 4:
                    passed = profile();
                    break;
                case 5:
                    passed = abort();
                    break;
                case 6:
                    passed = delayedInterrupts(false);
                    break;
                case 7:
                    passed = delayedInterrupts(true);
                    break;
                case 8:
                    log("435 pulses/axis at 1 Hz; real 32-bit counter wrap at about 429.497 s");
                    passed = constant({ 1s, 1s, 1s }, Counts{ 435U, 435U, 435U }) && startAndWait(437s) &&
                             final(State::Completed, Counts{ 435U, 435U, 435U });
                    break;
                default:
                    break;
            }
            return report(passed);
        }

      private:
        struct Progress
        {
            std::uint32_t calls{};
            hal::step::Status last;
            bool monotonic{ true };
        };
        auto check(bool condition, const char* message) -> bool
        {
            if (!condition) {
                log("CHECK FAILED: %s", message);
            }
            return condition;
        }
        auto idle() -> bool
        {
            if (!padsLow() || (TIM2->CR1 & TIM_CR1_CEN) != 0U || m_enable->read() != hal::gpio::Level::High) {
                return false;
            }
            for (const auto& direction : m_directions) {
                if (direction->read() != hal::gpio::Level::Low) {
                    return false;
                }
            }
            return true;
        }
        auto constant(std::array<std::chrono::microseconds, 3> periods,
                      std::optional<Counts> counts = std::nullopt) -> bool
        {
            for (std::size_t i = 0; i < 3U; ++i) {
                const std::optional<hal::step::PulseCount> count{ counts ? std::optional{ (*counts)[i] }
                                                                         : std::nullopt };
                if (!check(m_axes[i]->prepare({ periods[i], 5us }, count).has_value(), "prepare")) {
                    return false;
                }
            }
            return true;
        }
        auto wait(std::chrono::milliseconds timeout) -> bool
        {
            const auto until{ Clock::now() + timeout };
            Counts previous{};
            while (true) {
                const auto status{ m_generator->status() };
                for (std::size_t i = 0; i < 3U; ++i) {
                    if (!check(status.pulses[i] >= previous[i], "monotonic polled pulse count")) {
                        return false;
                    }
                }
                previous = status.pulses;
                if (status.state != State::Running) {
                    return check(status.state == State::Completed, "normal completion");
                }
                if (!check(!m_button->isPressed(), "B1 abort (case cancelled)") ||
                    !check(Clock::now() < until, "completion timeout")) {
                    return false;
                }
                std::this_thread::sleep_for(1ms);
            }
        }
        auto startAndWait(std::chrono::milliseconds timeout) -> bool
        {
            return check(m_generator->start(1ms).has_value(), "start") && wait(timeout);
        }
        auto final(State expected, std::optional<Counts> counts = std::nullopt) -> bool
        {
            const auto before{ m_generator->status() };
            // Check quiescence and allow a pending completion notification to run.
            std::this_thread::sleep_for(20ms);
            const auto after{ m_generator->status() };
            Progress progress;
            {
                const hal::stm32::InterruptGuard lock;
                progress = m_progress;
            }
            bool passed{ check(after.state == expected && after.counts_exact, "terminal state/exact count") &&
                         check(before == after, "counts/state remain unchanged after stopping") &&
                         check(idle(), "stopped counter, STEP/DIR low, enable high") &&
                         check(!counts || after.pulses == *counts, "expected pulse counts") &&
                         check(progress.calls > 0U && progress.monotonic && progress.last == after,
                               "batched callback reports monotonic counts and terminal state") };
            for (std::size_t i = 0; i < 3U; ++i) {
                const auto count{ m_axes[i]->pulseCount() };
                passed &= check(count && *count == after.pulses[i], "axis count agrees with group status");
            }
            return passed;
        }
        auto profile() -> bool
        {
            std::vector<hal::step::Timing> timings;
            timings.reserve(1024U);
            for (unsigned i = 0; i < 1024U; ++i) {
                const auto offset{ i < 512U ? i : 1023U - i };
                timings.push_back({ std::chrono::microseconds{ 1000U - offset }, 5us });
            }
            for (auto& axis : m_axes) {
                if (!check(axis->prepareSequence(timings).has_value(), "prepare copied profile")) {
                    return false;
                }
            }
            // The source need not survive DMA execution.
            timings.clear();
            timings.shrink_to_fit();
            return startAndWait(2s) && final(State::Completed, Counts{ 1024U, 1024U, 1024U });
        }
        auto abort() -> bool
        {
            if (!constant({ 100us, 200us, 400us }) ||
                !check(m_generator->start(1ms).has_value(), "start continuous train")) {
                return false;
            }
            std::this_thread::sleep_for(100ms);
            const auto result{ m_generator->stop() };
            return check(result.pulses[0] > 0U && result.pulses[1] > 0U && result.pulses[2] > 0U,
                         "continuous train made progress") &&
                   final(State::Stopped);
        }
        auto delayedInterrupts(bool underrun) -> bool
        {
            const std::optional<Counts> counts{ underrun ? std::nullopt
                                                         : std::optional{ Counts{ 1000U, 1000U, 1000U } } };
            if (!constant({ 10us, 20us, 40us }, counts) ||
                !check(m_generator->start(1ms).has_value(), "start IRQ delay test")) {
                return false;
            }
            bool stopped{}, pending{};
            {
                const WithholdDmaInterrupts hold;
                busyWait(underrun ? 20ms : 4500us);
                // Read before restoring IRQs or asking the generator to service
                // anything. This is evidence of the independent DMA stop path.
                stopped = (TIM2->CR1 & TIM_CR1_CEN) == 0U;
                pending = (DMA1->LISR & DMA_LISR_TCIF0) != 0U;
                if (underrun) {
                    hardware_step_test_guard_stopped = stopped ? 1U : 0U;
                }
            }
            if (underrun) {
                return check(stopped, "hardware stopped TIM2 while all DMA IRQs were masked") &&
                       check(pending, "DMA buffer completed while its IRQ was masked") &&
                       final(State::Underrun, Counts{ 512U, 256U, 128U });
            }
            return check(!stopped && pending, "delayed refill, counter still running") && wait(100ms) &&
                   final(State::Completed, Counts{ 1000U, 1000U, 1000U });
        }
        auto report(bool passed) -> bool
        {
            const auto status{ m_generator->stop() };
            Progress progress;
            {
                const hal::stm32::InterruptGuard lock;
                progress = m_progress;
            }
            for (std::size_t i = 0; i < 3U; ++i) {
                hardware_step_test_pulses[i] = status.pulses[i];
            }
            passed &= idle();
            hardware_step_test_status = passed ? 0x600D600DU : 0xBAD00000U | hardware_step_test_case;
            m_yellow->turnOff();
            (passed ? m_green : m_red)->turnOn();
            log("%s case=%lu state=%s exact=%u counts=%llu,%llu,%llu callbacks=%lu guard_stopped=%lu",
                passed ? "PASS" : "FAIL",
                static_cast<unsigned long>(hardware_step_test_case),
                stateName(status.state),
                status.counts_exact ? 1U : 0U,
                static_cast<unsigned long long>(status.pulses[0]),
                static_cast<unsigned long long>(status.pulses[1]),
                static_cast<unsigned long long>(status.pulses[2]),
                static_cast<unsigned long>(progress.calls),
                static_cast<unsigned long>(hardware_step_test_guard_stopped));
            hardware_step_test_complete();
            return passed;
        }

        Progress m_progress;
        std::shared_ptr<hal::IDigitalOutput> m_enable;
        std::array<std::shared_ptr<hal::IDigitalOutput>, 3> m_directions;
        std::shared_ptr<hal::IStepGenerator> m_generator;
        std::array<std::shared_ptr<hal::IStepOutput>, 3> m_axes;
        std::shared_ptr<hal::device::IButton> m_button;
        std::shared_ptr<hal::device::ILed> m_green, m_red, m_yellow;
    };

    void menu()
    {
        log("1=1/2/3 pulses  2=independent rates  3=100 kHz x3  4=profile");
        log("5=abort continuous  6=late IRQ  7=underrun  8=wrap (~7m15s)");
        log("9=run cases 1..7  h=help; enter one command then Enter");
        log("EN_N remains HIGH and DIR LOW. PASS checks software; verify waveforms separately.");
    }
}

int main()
{
    try {
        Bench bench;
        if (!bench.initialize()) {
            log("FATAL: cannot claim/configure the test board resources");
            return 1;
        }
        log("READY: STEP PA0/PB10/PB11; shared enable PE15 is disabled");
        menu();
        while (true) {
            std::array<char, 32> line{};
            if (std::fgets(line.data(), static_cast<int>(line.size()), stdin) == nullptr) {
                log("FATAL: serial read failed");
                return 1;
            }
            // Discard an overlong line as a unit, never execute its suffix.
            if (std::strchr(line.data(), '\n') == nullptr) {
                int c;
                do {
                    c = std::getchar();
                } while (c != '\n' && c != EOF);
                menu();
                continue;
            }
            // The console normalizes CR/CRLF to LF. Accept one command only.
            if (line[1] != '\n') {
                if (line[0] != '\n') {
                    menu();
                }
                continue;
            }
            if (line[0] >= '1' && line[0] <= '8') {
                static_cast<void>(bench.run(static_cast<unsigned>(line[0] - '0')));
            }
            else if (line[0] == '9') {
                bool passed{ true };
                for (unsigned test = 1; passed && test <= 7; ++test) {
                    passed = bench.run(test);
                }
                log("SUITE %s (cases 1..7; wrap is separate)", passed ? "PASS" : "FAIL");
            }
            else {
                menu();
            }
            log("READY for command");
        }
    } catch (const std::exception& error) {
        log("FATAL: %s", error.what());
        return 1;
    }
}
