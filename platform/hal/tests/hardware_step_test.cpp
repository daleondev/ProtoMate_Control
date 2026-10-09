#include "hal/tests/support/SignalOnlyDriver.hpp"
#include "clock_reference.hpp"
#include "control/StepperMotor.hpp"
#include "hal/board/board.hpp"
#include "hal/hal.hpp"
#include "hal/stm32/InterruptGuard.hpp"
#include "runtime/synchronization/Notification.hpp"
#include "runtime/thread.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <future>
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
                                   DMA1_Stream3_IRQn,
                                   TIM7_IRQn };

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

    // Mask the four DMA vectors and the completion monitor. DMA and TIM2 keep running;
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
        std::array<bool, 5> m_enabled{};
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
                    m_progress.monotonic &= m_progress.last.axes[i] != State::Running ||
                                            status.pulses[i] >= m_progress.last.pulses[i];
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
                case 11:
                    passed = independent();
                    break;
                case 12:
                    if (check(m_generator->start().has_value(), "start idle timebase")) {
                        // Test-only: jump an idle counter to 5 ms before its real wrap.
                        TIM2->CNT = 0xFFFFFFFEU - 50'000U;
                        passed = constant({ 100us, 200us, 400us }, Counts{ 64U, 64U, 64U }) &&
                                 startAndWait(100ms) && final(State::Completed, Counts{ 64U, 64U, 64U });
                    }
                    break;
                case 13:
                    passed = notifications();
                    break;
                case 14:
                    passed = motors();
                    break;
                case 15:
                case 16:
                case 17:
                case 18:
                case 19:
                    passed = plannedMotors(test);
                    break;
                default:
                    break;
            }
            return report(passed);
        }

        void runClockReference()
        {
            hardware_step_test_case = 10U; // UART command 'c'.
            hardware_step_test_status = 0x52554E00U;
            m_green->turnOff();
            m_red->turnOff();
            m_yellow->turnOn();
            static_cast<void>(m_generator->stop());
            log("CLOCK: TIM2 versus RTC/LSE; three 10-second windows (~31 s)");
            log("CLOCK: STEP stays LOW, EN_N HIGH; no scope needed; B1 cancels");
            log("CLOCK: HSE_VALUE=%lu is an assumption, not a measured frequency",
                static_cast<unsigned long>(HSE_VALUE));
            log("CLOCK registers: BDCR=%08lx RTC_CR=%08lx PRER=%08lx ISR=%08lx CALR=%08lx",
                static_cast<unsigned long>(RCC->BDCR),
                static_cast<unsigned long>(RTC->CR),
                static_cast<unsigned long>(RTC->PRER),
                static_cast<unsigned long>(RTC->ISR),
                static_cast<unsigned long>(RTC->CALR));

            std::array<clock_reference::Boundary, 4> boundaries{};
            const bool valid{ acquireClockReference(boundaries) };
            const bool stopped{ idle() };
            if (valid && stopped) {
                for (unsigned i = 0; i < 3U; ++i) {
                    const auto result{ clock_reference::measure(boundaries[i], boundaries[i + 1U]) };
                    log("CLOCK window=%u rtc_seconds=10 tim2_ticks=%.0f timer_hz=%.1f "
                        "error_ppm=%+.1f sampling_bound_ppm=%.1f screen=%s",
                        i + 1U,
                        result.ticks,
                        result.hz,
                        result.error_ppm,
                        result.sampling_bound_ppm,
                        clock_reference::screenName(result.screen));
                }
                log("CLOCK VALID: positive ppm means TIM2 is fast relative to LSE; "
                    "10000 ppm = 1 percent");
                log("CLOCK: sampling bounds exclude LSE crystal tolerance; VALID is not timing PASS");
            }
            else {
                log("CLOCK INVALID: no frequency conclusion; inspect CHECK FAILED above");
            }
            hardware_step_test_status = valid && stopped ? 0x600D600DU : 0xBAD0000AU;
            m_yellow->turnOff();
            (valid && stopped ? m_green : m_red)->turnOn();
            hardware_step_test_complete();
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

        auto rtcReferenceReady() const -> bool
        {
            // A clock derived from the main oscillator would not be an
            // independent reference. Reject calendar adjustments as well.
            return (RCC->BDCR & (RCC_BDCR_RTCSEL | RCC_BDCR_LSERDY | RCC_BDCR_RTCEN)) ==
                     (RCC_RTCCLKSOURCE_LSE | RCC_BDCR_LSERDY | RCC_BDCR_RTCEN) &&
                   RTC->PRER == ((127U << RTC_PRER_PREDIV_A_Pos) | 255U) &&
                   (RTC->CR & (RTC_CR_BYPSHAD | RTC_CR_REFCKON)) == 0U && RTC->CALR == 0U &&
                   // INITS reflects year != 00. The default year 2000 can
                   // legitimately leave it clear while seconds advance.
                   (RTC->ISR & (RTC_ISR_INIT | RTC_ISR_SHPF | RTC_ISR_RECALPF | RTC_ISR_RSF)) == RTC_ISR_RSF;
        }

        auto acquireClockReference(std::array<clock_reference::Boundary, 4>& boundaries) -> bool
        {
            if (!check(idle() && !m_button->isPressed(),
                       "clock test requires idle outputs and released B1") ||
                !check(rtcReferenceReady(), "RTC must use ready, unadjusted LSE with 127/255 prescalers") ||
                !check(TIM2->PSC == 23U && TIM2->ARR == hal::util::step_arr && TIM2->SMCR == 0U &&
                         (TIM2->CR1 & (TIM_CR1_DIR | TIM_CR1_CMS | TIM_CR1_OPM)) == 0U,
                       "TIM2 must be the configured 10 MHz up-counter")) {
                return false;
            }
            for (auto* stream : { DMA1_Stream0, DMA1_Stream1, DMA1_Stream2, DMA1_Stream3 }) {
                if (!check((stream->CR & DMA_SxCR_EN) == 0U, "step DMA must be stopped")) {
                    return false;
                }
            }
            // Bench-only counter measurement. The generator is stopped and its
            // STEP owners hold the pads low as GPIOs. Disconnect every channel
            // and request before starting the counter; no edges are generated.
            struct StopCounter
            {
                ~StopCounter()
                {
                    CLEAR_BIT(TIM2->CR1, TIM_CR1_CEN);
                    __DSB();
                    TIM2->SR = 0U;
                }
            } stop_counter;
            TIM2->DIER = 0U;
            TIM2->CCER = 0U;
            TIM2->CNT = 0U;
            TIM2->SR = 0U;
            SET_BIT(TIM2->CR1, TIM_CR1_CEN);

            clock_reference::Tracker tracker;
            unsigned transitions{};
            bool aligned{};
            // TIM5/steady_clock only bounds a stalled test. It never supplies
            // the measurement's elapsed seconds: those come solely from RTC.
            const auto deadline{ Clock::now() + 40s };
            auto last_transition{ Clock::now() };
            while (true) {
                std::this_thread::sleep_for(1ms); // >1 RTCCLK between shadow reads (RM0433).
                if (!check(!m_button->isPressed(), "B1 cancelled clock test") ||
                    !check(rtcReferenceReady(), "RTC reference changed or stopped") ||
                    !check(Clock::now() < deadline && Clock::now() - last_transition < 2s,
                           "RTC transition timeout") ||
                    !check(padsLow() && m_enable->read() == hal::gpio::Level::High &&
                             (TIM2->CR1 & TIM_CR1_CEN) != 0U,
                           "clock test output/counter state")) {
                    return false;
                }
                clock_reference::Sample sample;
                std::uint32_t seconds_bcd{};
                {
                    const hal::stm32::InterruptGuard lock;
                    sample.before = TIM2->CNT;
                    // Only seconds from one atomic TR read are needed. No
                    // SSR/date tuple, hence no calendar-lock erratum ambiguity.
                    seconds_bcd = RTC->TR & (RTC_TR_ST | RTC_TR_SU);
                    static_cast<void>(RTC->DR); // Unlock the shadow registers.
                    sample.after = TIM2->CNT;
                }
                if (!check((seconds_bcd & 0xFU) <= 9U, "valid RTC BCD seconds")) {
                    return false;
                }
                sample.second = (seconds_bcd >> 4U) * 10U + (seconds_bcd & 0xFU);
                const auto observation{ tracker.observe(sample) };
                if (!check(
                      observation != clock_reference::Observation::Invalid,
                      "RTC jumped or sampling gap exceeded nominal 5 ms; rerun without debugger pauses")) {
                    return false;
                }
                if (observation == clock_reference::Observation::Boundary) {
                    last_transition = Clock::now();
                    if (!aligned) {
                        boundaries[0] = tracker.boundary();
                        aligned = true;
                    }
                    else if (++transitions % clock_reference::window_seconds == 0U) {
                        boundaries[transitions / clock_reference::window_seconds] = tracker.boundary();
                        if (transitions == 3U * clock_reference::window_seconds) {
                            return true;
                        }
                    }
                }
            }
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
        auto notifications() -> bool
        {
            std::array<runtime::Notification, 3> events;
            std::array<unsigned, 3> calls{}, from_isr{};
            bool masks_preserved{ true };
            struct Retire
            {
                decltype(m_axes)& axes;
                ~Retire()
                {
                    for (auto& axis : axes)
                        static_cast<void>(axis->setCompletionCallback({}));
                }
            } retire{ m_axes };
            for (unsigned i = 0; i < 3; ++i) {
                if (!m_axes[i]->setCompletionCallback([&, i](const auto&) noexcept {
                    ++calls[i];
                    if (__get_IPSR() != 0U)
                        ++from_isr[i];
                    const auto mask{ __get_PRIMASK() };
                    events[i].signal();
                    masks_preserved &= mask == 1U && __get_PRIMASK() == mask;
                }))
                    return false;
            }
            if (!constant({ 10us, 20us, 40us }, Counts{ 513U, 777U, 1000U }) || !startMoves())
                return false;
            for (auto& event : events) {
                if (!check(event.waitUntil(Clock::now() + 1s), "completion wakes blocked waiter"))
                    return false;
            }
            if (!check(calls == std::array<unsigned, 3>{ 1, 1, 1 } && from_isr == calls && masks_preserved,
                       "one ISR callback per completed axis, interrupts remain masked") ||
                !final(State::Completed, Counts{ 513U, 777U, 1000U }))
                return false;

            // Wake a HIGHER priority worker from an API-triggered callback while
            // the caller still owns an outer PRIMASK lock. It must not run yet.
            std::promise<bool> awake;
            auto result{ awake.get_future() };
            std::atomic_bool resumed{};
            auto waiter{ runtime::thread::create_jthread(
              { .name = "event test", .priority = 4, .stack_size = 8192U }, [&] {
                const auto received{ events[0].waitUntil(Clock::now() + 1s) };
                resumed = true;
                awake.set_value(received);
            }) };
            if (!m_axes[0]->prepare({ 1ms, 5us }) || !m_axes[0]->start())
                return false;
            bool deferred{};
            {
                const hal::stm32::InterruptGuard lock;
                static_cast<void>(m_axes[0]->stop());
                deferred = __get_PRIMASK() == 1U && !resumed.load();
            }
            const bool delivered{ result.wait_for(1s) == std::future_status::ready && result.get() };
            waiter.join();
            log("NOTIFICATIONS ISR=%u,%u,%u thread=%u mask_preserved=%u deferred=%u",
                from_isr[0],
                from_isr[1],
                from_isr[2],
                calls[0] - from_isr[0],
                masks_preserved,
                deferred);
            return check(delivered && deferred && masks_preserved && calls[0] == 2U,
                         "masked thread callback defers rescheduling until unlock");
        }
        // All profile cases use Backward moves so an unconnected reference
        // switch remains protective without needing extra bench jumpers.
        auto plannedMotors(unsigned test) -> bool
        {
            m_directions = {};
            struct RestoreDirections
            {
                decltype(m_directions)& directions;
                ~RestoreDirections()
                {
                    for (unsigned i = 0; i < directions.size(); ++i)
                        directions[i] =
                          hal::board::createStepperDirectionOutput(static_cast<hal::board::MotorId>(i));
                }
            } restore{ m_directions };
            using enum StepperMotor::Result;
            using enum StepperMotor::BufferMode;
            std::array<std::unique_ptr<StepperMotor>, 3> motors;
            for (unsigned i = 0; i < 3; ++i) {
                motors[i] = std::make_unique<StepperMotor>(
                  static_cast<hal::board::MotorId>(i), 0_deg, 1.8_deg, 16U, m_generator, hal::test::signalOnlyDriver());
                if (!motors[i]->setMotionDefaults({ 1440_deg_s2, 2160_deg_s2, 14400_deg_s3 }))
                    return false;
            }
            if (!m_generator->start())
                return false;
            std::array<std::future<StepperMotor::Result>, 3> active, following;
            for (unsigned i = 0; i < 3; ++i) {
                if (test == 15) {
                    active[i] = motors[i]->moveRel(-360_deg - 180_deg * i, 120_rpm + 60_rpm * i);
                }
                else if (test == 16) {
                    active[i] = motors[i]->moveRel(-360_deg, 120_rpm);
                    following[i] = motors[i]->moveRel(-180_deg,
                                                      60_rpm,
                                                      0_rad_s2,
                                                      0_rad_s2,
                                                      0_rad_s3,
                                                      i == 0   ? BlendingLow
                                                      : i == 1 ? BlendingHigh
                                                               : Buffered);
                }
                else if (test == 17) {
                    active[i] = motors[i]->moveRel(-720_deg, 120_rpm);
                }
                else {
                    active[i] = motors[i]->moveRel(test == 19 ? -14400_deg : -7200_deg,
                                                   1875_rpm,
                                                   100000_deg_s2,
                                                   100000_deg_s2,
                                                   1000000_deg_s3);
                }
                if (test == 19 && i < 2)
                    std::this_thread::sleep_for(300ms);
            }
            if (test == 17 || test == 19) {
                std::this_thread::sleep_for(test == 19 ? 100ms : 250ms);
                const auto update{ motors[1]->setVelocity(test == 19 ? 1000_rpm : 240_rpm) };
                if (!check(update.has_value(), "jerk-limited live velocity update accepted"))
                    return false;
                log("PLANNED velocity first_affected_pulse=%llu", static_cast<unsigned long long>(*update));
            }
            for (unsigned i = 0; i < 3; ++i) {
                if (!check(active[i].wait_for(6s) == std::future_status::ready &&
                             active[i].get() == Completed,
                           "profile command completed"))
                    return false;
                if (test == 16 && !check(following[i].wait_for(6s) == std::future_status::ready &&
                                           following[i].get() == Completed,
                                         "successor completed"))
                    return false;
                const auto degrees{ test == 15   ? 360.0 + 180.0 * i
                                    : test == 16 ? 540.0
                                    : test == 17 ? 720.0
                                    : test == 19 ? 14400.0
                                                 : 7200.0 };
                if (!check(std::abs(motors[i]->position().get<pnm::units::AngleUnits::deg>() + degrees) <
                               1e-6 &&
                             motors[i]->velocity() == 0_rpm,
                           "profile final position and velocity"))
                    return false;
            }
            log("PLANNED test=%u cumulative_counts=%u,%u,%u",
                test,
                test == 15   ? 3200U
                : test == 16 ? 4800U
                : test == 17 ? 6400U
                : test == 19 ? 128000U
                             : 64000U,
                test == 15   ? 4800U
                : test == 16 ? 4800U
                : test == 17 ? 6400U
                : test == 19 ? 128000U
                             : 64000U,
                test == 15   ? 6400U
                : test == 16 ? 4800U
                : test == 17 ? 6400U
                : test == 19 ? 128000U
                             : 64000U);
            return check(padsLow() && m_enable->read() == hal::gpio::Level::High,
                         "profile outputs low and drivers disabled");
        }
        auto motors() -> bool
        {
            // Motor objects own DIR/reference resources; release the bench's DIR
            // handles for this case and restore the usual idle wiring afterward.
            m_directions = {};
            struct RestoreDirections
            {
                decltype(m_directions)& directions;
                ~RestoreDirections()
                {
                    for (unsigned i = 0; i < directions.size(); ++i)
                        directions[i] =
                          hal::board::createStepperDirectionOutput(static_cast<hal::board::MotorId>(i));
                }
            } restore{ m_directions };
            using namespace pnm::units::literals;
            using enum StepperMotor::Result;
            // Backward motion is permitted with open reference inputs, so the
            // bare-board bench needs no switch jumpers. Drivers stay disabled.
            StepperMotor first{ hal::board::MotorId::Motor2, 0_deg, 1.8_deg, 16U, m_generator, hal::test::signalOnlyDriver() };
            StepperMotor second{ hal::board::MotorId::Motor3, 0_deg, 1.8_deg, 16U, m_generator, hal::test::signalOnlyDriver() };
            if (!m_generator->start())
                return false;
            auto a{ first.moveRel(-90_deg, 300_rpm) };
            auto b{ second.moveRel(-100_rev, 300_rpm, 0.02_s) };
            if (!check(a.wait_for(1s) == std::future_status::ready && a.get() == Completed,
                       "motor future completes without polling") ||
                !check(b.wait_for(1s) == std::future_status::ready && b.get() == TimedOut,
                       "independent motor deadline wakes waiter"))
                return false;
            if (!check(m_axes[1]->pulseCount() == 800U &&
                         std::abs(first.position().get<pnm::units::AngleUnits::deg>() + 90.0) < 1e-9 &&
                         first.velocity() == 0_rpm && second.velocity() == 0_rpm,
                       "final commanded position accounts for all pulses"))
                return false;
            auto moving{ first.moveRel(-100_rev, 9.375_rpm) };
            std::this_thread::sleep_for(150ms);
            const auto boundary{ first.setVelocity(18.75_rpm) };
            const auto pending{ m_axes[1]->status() };
            if (!check(boundary.has_value() && pending.counts_exact && pending.pulses < *boundary &&
                         std::abs(first.velocity().get<pnm::units::AngularVelocityUnits::rpm>() + 9.375) <
                           0.001,
                       "current velocity excludes pending DMA timing change"))
                return false;
            std::this_thread::sleep_for(1200ms);
            const auto executed{ m_axes[1]->status() };
            if (!check(executed.counts_exact && executed.pulses >= *boundary &&
                         std::abs(first.velocity().get<pnm::units::AngularVelocityUnits::rpm>() + 18.75) <
                           0.001,
                       "current signed velocity follows executed timing"))
                return false;
            first.stop();
            const auto stopped_position{ first.position() };
            if (!check(first.velocity() == 0_rpm && moving.wait_for(1s) == std::future_status::ready &&
                         moving.get() == Stopped && first.position() == stopped_position,
                       "stop publishes final motor state before worker completion"))
                return false;
            for (unsigned i = 0; i < 10; ++i) {
                auto old{ first.moveRel(-100_rev, 300_rpm) };
                auto next{ first.moveRel(-1.125_deg, 300_rpm) };
                if (!check(old.wait_for(1s) == std::future_status::ready && old.get() == Stopped &&
                             next.wait_for(1s) == std::future_status::ready && next.get() == Completed,
                           "replacement wakes cancellation and completes next motion"))
                    return false;
            }
            log("MOTORS finite=800 replacement=10x10 timeout=PASS event_wait=PASS current_state=PASS");
            return check(m_axes[1]->pulseCount() == 10U && m_generator->status().state == State::Running,
                         "replacement count and persistent timebase");
        }
        auto motionStatus() -> hal::step::Status
        {
            auto status{ m_generator->status() };
            if (status.state == State::Running) {
                bool running{}, stopped{};
                for (const auto axis : status.axes) {
                    running |= axis == State::Running;
                    stopped |= axis == State::Stopped;
                }
                if (!running)
                    status.state = stopped ? State::Stopped : State::Completed;
            }
            return status;
        }
        auto startMoves() -> bool
        {
            if (!m_generator->start())
                return false;
            for (auto& axis : m_axes) {
                if (axis->status().state == State::Ready && !axis->start())
                    return false;
            }
            return true;
        }
        auto independent() -> bool
        {
            if (!check(m_generator->start().has_value(), "start idle timebase") ||
                !check(m_axes[0]->prepare({ 10us, 5us }, 100000U).has_value(), "prepare M1") ||
                !check(m_axes[0]->start().has_value(), "start M1"))
                return false;
            std::this_thread::sleep_for(100ms);
            if (!check(m_axes[1]->prepare({ 40us, 5us }, 20000U).has_value(), "prepare M2 while M1 runs") ||
                !check(m_axes[1]->start().has_value(), "start M2 later"))
                return false;
            std::this_thread::sleep_for(100ms);
            const auto update{ m_axes[1]->updateTiming({ 20us, 5us }) };
            if (!check(update.has_value(), "change M2 speed without restarting"))
                return false;
            log("INDEPENDENT M2 new_period_us=20 first_affected_pulse=%llu",
                static_cast<unsigned long long>(*update));
            if (!check(m_axes[2]->prepare({ 100us, 5us }).has_value(), "prepare M3") ||
                !check(m_axes[2]->start().has_value(), "start M3"))
                return false;
            std::this_thread::sleep_for(100ms);
            const auto stopped{ m_axes[2]->stop() };
            log("INDEPENDENT M3 first_burst_pulses=%llu", static_cast<unsigned long long>(stopped.pulses));
            if (!check(stopped.state == State::Stopped && stopped.counts_exact, "stop only M3") ||
                !check(m_axes[0]->status().state == State::Running &&
                         m_axes[1]->status().state == State::Running,
                       "M1/M2 continue through M3 stop"))
                return false;
            std::this_thread::sleep_for(50ms);
            if (!check(m_axes[2]->prepare({ 50us, 5us }, 333U).has_value(), "prepare M3 restart") ||
                !check(m_axes[2]->start().has_value(), "restart only M3"))
                return false;
            return wait(2s) && final(State::Completed, Counts{ 100000U, 20000U, 333U });
        }
        auto wait(std::chrono::milliseconds timeout) -> bool
        {
            const auto until{ Clock::now() + timeout };
            Counts previous{};
            while (true) {
                const auto status{ motionStatus() };
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
            return check(startMoves(), "start") && wait(timeout);
        }
        auto final(State expected, std::optional<Counts> counts = std::nullopt) -> bool
        {
            const auto before{ motionStatus() };
            // Check quiescence and allow a pending completion notification to run.
            std::this_thread::sleep_for(20ms);
            const auto after{ motionStatus() };
            Progress progress;
            {
                const hal::stm32::InterruptGuard lock;
                progress = m_progress;
            }
            bool passed{
                check(after.state == expected && after.counts_exact, "terminal state/exact count") &&
                check(before == after, "counts/state remain unchanged after stopping") &&
                check(padsLow() && m_enable->read() == hal::gpio::Level::High, "STEP low, enable high") &&
                check(expected == State::Underrun || expected == State::Stopped ||
                        (TIM2->CR1 & TIM_CR1_CEN) != 0U,
                      "idle timebase remains running") &&
                check(!counts || after.pulses == *counts, "expected pulse counts") &&
                check(progress.calls > 0U && progress.monotonic && progress.last == m_generator->status(),
                      "batched callback reports monotonic counts and terminal state")
            };
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
            if (!constant({ 100us, 200us, 400us }) || !check(startMoves(), "start continuous train")) {
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
            if (!constant({ 10us, 20us, 40us }, counts) || !check(startMoves(), "start IRQ delay test")) {
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
                       check(m_generator->status().pulses[0] == 512U,
                             "fast axis stops at its two-buffer horizon") &&
                       final(State::Underrun);
            }
            return check(!stopped && pending, "delayed refill, counter still running") && wait(100ms) &&
                   final(State::Completed, Counts{ 1000U, 1000U, 1000U });
        }
        auto report(bool passed) -> bool
        {
            const auto status{ motionStatus() };
            static_cast<void>(m_generator->stop());
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
        log("9=run cases 1..7  i=independent start/stop/live speed  w=quick counter wrap  h=help");
        log("n=ISR/thread notification wait  m=StepperMotor completion/cancel/timeout");
        log("p=three ramps  b=Low/High/Buffered  v=live ramp update  a=100kHz profile stress x3");
        log("c=TIM2 clock versus RTC crystal (~31 s, no STEP pulses)");
        log("s=staggered 100kHz profiles with a live slowdown");
        log("EN_N remains HIGH. DIR stays LOW in the bench cases. Verify waveforms separately.");
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
            else if (line[0] == 'i') {
                static_cast<void>(bench.run(11U));
            }
            else if (line[0] == 'w') {
                static_cast<void>(bench.run(12U));
            }
            else if (line[0] == 'n') {
                static_cast<void>(bench.run(13U));
            }
            else if (line[0] == 'm') {
                static_cast<void>(bench.run(14U));
            }
            else if (line[0] == 'p' || line[0] == 'b' || line[0] == 'v' || line[0] == 'a') {
                static_cast<void>(bench.run(line[0] == 'p'   ? 15U
                                            : line[0] == 'b' ? 16U
                                            : line[0] == 'v' ? 17U
                                                             : 18U));
            }
            else if (line[0] == 's') {
                static_cast<void>(bench.run(19U));
            }
            else if (line[0] == '9') {
                bool passed{ true };
                for (unsigned test = 1; passed && test <= 7; ++test) {
                    passed = bench.run(test);
                }
                log("SUITE %s (cases 1..7; wrap is separate)", passed ? "PASS" : "FAIL");
            }
            else if (line[0] == 'c') {
                bench.runClockReference();
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
