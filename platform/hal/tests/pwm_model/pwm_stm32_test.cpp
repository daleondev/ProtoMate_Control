#include "hal/drivers/impl/stm32/PwmOutput.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace
{
    using namespace std::chrono_literals;
    class Pin final : public hal::IDigitalOutput
    {
      public:
        auto read() const noexcept -> hal::gpio::Level override { return hal::gpio::Level::Low; }
        auto write(hal::gpio::Level) noexcept -> void override {}
        auto toggle() noexcept -> void override {}
    };

    class HardwarePwm : public testing::TestWithParam<std::uint8_t>
    {
      protected:
        TIM_TypeDef* timer{};
        GPIO_TypeDef* port{};
        std::unique_ptr<hal::PwmOutput> output;
        std::uint64_t ticks{}, rises{}, falls{}, high_started{}, high_ticks{};
        bool was_high{};

        void SetUp() override
        {
            using enum hal::gpio::Port;
            const auto id{ GetParam() };
            timer = id == 1U ? &timer1 : id == 4U ? &timer4 : &timer8;
            port = id == 1U ? GPIOE : id == 4U ? GPIOD : GPIOC;
            // EGR retains its self pointer; reset only through normal registers.
            timer->CR1 = timer->DIER = 0U;
            timer->SR = 0U;
            const hal::pwm::Configuration route = id == 1U   ? hal::pwm::Configuration{ 1U, 1U, { E, 9U } }
                                                  : id == 4U ? hal::pwm::Configuration{ 4U, 3U, { D, 14U } }
                                                             : hal::pwm::Configuration{ 8U, 1U, { C, 6U } };
            output = std::make_unique<hal::PwmOutput>(
              route, 1U, hal::detail::TimerLease{ id }, std::make_shared<Pin>());
            ASSERT_TRUE(output->configure({ 100ns, 50ns })); // 24 ticks, 12 ticks high.
        }
        bool high() const { return port->mode == GPIO_MODE_AF_PP ? timer->high() : port->latch != 0U; }
        void observe()
        {
            const bool now{ high() };
            if (now && !was_high) {
                ++rises;
                high_started = ticks;
            }
            else if (!now && was_high) {
                ++falls;
                high_ticks = ticks - high_started;
            }
            was_high = now;
        }
        void dispatch()
        {
            hal::PwmOutput::dispatchInterrupt(GetParam());
            observe();
        }
        void advance(std::uint64_t count, bool service)
        {
            for (std::uint64_t i{}; i < count; ++i) {
                ++ticks;
                timer->tick();
                observe();
                if (service && (timer->SR & timer->DIER) != 0U) {
                    dispatch();
                }
            }
        }
    };
    INSTANTIATE_TEST_SUITE_P(AllStepTimers, HardwarePwm, testing::Values(1U, 4U, 8U));
}

TEST_P(HardwarePwm, DelayedInterruptsCannotEmitExtraPulsesOrShortenTheFinalHighPhase)
{
    std::uint64_t callbacks{};
    ASSERT_TRUE(output->setPulseCallback([&](std::uint64_t count) noexcept { callbacks = count; }));
    ASSERT_TRUE(output->startPulses(3U));
    EXPECT_EQ(output->pulseCount(), 0U); // Software UG/arming must not count.
    EXPECT_FALSE(high());
    for (std::uint64_t expected{ 1U }; expected <= 3U; ++expected) {
        advance(2400U, false); // 100 nominal periods with EVERY interrupt withheld.
        EXPECT_EQ(rises, expected);
        EXPECT_EQ(falls, expected);
        EXPECT_EQ(high_ticks, 12U);
        EXPECT_FALSE(high());
        EXPECT_EQ(timer->CR1 & TIM_CR1_CEN, 0U);
        EXPECT_EQ(output->pulseCount(), expected); // Includes unserviced compare.
        dispatch();                                // Both compare and update flags are pending.
        EXPECT_EQ(callbacks, expected);
        EXPECT_EQ(output->isRunning(), expected != 3U);
        dispatch(); // A stale second vector cannot count/rearm again.
    }
    advance(2400U, true);
    EXPECT_EQ(rises, 3U);
    EXPECT_EQ(falls, 3U);
    EXPECT_EQ(timer->DIER, 0U);
}

TEST_P(HardwarePwm, ServicesSeparateRisingAndFallingInterrupts)
{
    std::uint64_t callbacks{};
    ASSERT_TRUE(output->setPulseCallback([&](std::uint64_t count) noexcept { callbacks = count; }));
    ASSERT_TRUE(output->startPulses(1U));
    advance(12U, true);
    EXPECT_TRUE(high());
    EXPECT_EQ(callbacks, 1U);
    EXPECT_EQ(output->pulseCount(), 1U);
    EXPECT_TRUE(output->isRunning());
    advance(11U, true);
    EXPECT_TRUE(high());
    EXPECT_TRUE(output->isRunning());
    advance(1U, true);
    EXPECT_FALSE(high());
    EXPECT_FALSE(output->isRunning());
    EXPECT_EQ(callbacks, 1U);
    EXPECT_EQ(high_ticks, 12U);
}

TEST_P(HardwarePwm, AbortDrainsAPendingRisingEdgeOnceAndRestartsCleanly)
{
    std::uint64_t callbacks{};
    ASSERT_TRUE(output->setPulseCallback([&](std::uint64_t) noexcept { ++callbacks; }));
    ASSERT_TRUE(output->startPulses(5U));
    advance(14U, false); // Stop within the first high phase, before its compare ISR.
    EXPECT_TRUE(high());
    EXPECT_EQ(output->pulseCount(), 1U);
    ASSERT_TRUE(output->stop());
    observe();
    EXPECT_FALSE(high());
    EXPECT_EQ(output->pulseCount(), 1U);
    EXPECT_EQ(callbacks, 1U);
    dispatch();
    ASSERT_TRUE(output->stop());
    EXPECT_EQ(callbacks, 1U);
    ASSERT_TRUE(output->startPulses(1U));
    EXPECT_EQ(output->pulseCount(), 0U);
    advance(240U, true);
    EXPECT_EQ(output->pulseCount(), 1U);
    EXPECT_EQ(callbacks, 2U);
    EXPECT_FALSE(output->isRunning());
}

TEST_P(HardwarePwm, CountsWithoutACallbackAndCanReturnToContinuousPwm)
{
    ASSERT_TRUE(output->startPulses(2U));
    EXPECT_EQ(timer->DIER, TIM_IT_UPDATE); // No compare IRQ needed for counting.
    advance(240U, true);
    EXPECT_EQ(rises, 2U);
    EXPECT_EQ(falls, 2U);
    EXPECT_EQ(output->pulseCount(), 2U);
    ASSERT_TRUE(output->start());
    EXPECT_EQ(timer->CR1 & TIM_CR1_OPM, 0U);
    EXPECT_EQ(timer->DIER, 0U);
    EXPECT_EQ(timer->mode, TIM_OCMODE_PWM1);
    advance(240U, true);
    EXPECT_GT(rises, 3U);
    EXPECT_EQ(output->pulseCount(), 0U);
    EXPECT_TRUE(output->isRunning());
    ASSERT_TRUE(output->stop());
    observe();
    EXPECT_FALSE(high());
    ASSERT_TRUE(output->startPulses(1U));
    advance(240U, true);
    EXPECT_EQ(output->pulseCount(), 1U);
    EXPECT_FALSE(output->isRunning());
}

TEST_P(HardwarePwm, DestructionDoesNotCallBackAndFailedStartRetainsTheCount)
{
    ASSERT_TRUE(output->startPulses(2U));
    advance(240U, true);
    next_start_status = HAL_ERROR;
    EXPECT_FALSE(output->startPulses(1U));
    EXPECT_EQ(output->pulseCount(), 2U);
    EXPECT_FALSE(output->isRunning());
    EXPECT_FALSE(high());
    EXPECT_EQ(timer->DIER, 0U);
    std::uint64_t callbacks{};
    ASSERT_TRUE(output->setPulseCallback([&](std::uint64_t) noexcept { ++callbacks; }));
    ASSERT_TRUE(output->startPulses(10U));
    advance(14U, false);
    output.reset();
    EXPECT_EQ(callbacks, 0U);
    EXPECT_FALSE(high());
    EXPECT_EQ(timer->CR1 & TIM_CR1_CEN, 0U);
    EXPECT_EQ(timer->DIER, 0U);
    dispatch(); // Detached vector is harmless.
    EXPECT_EQ(callbacks, 0U);
}
