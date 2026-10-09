#include "hal/drivers/detail/PulseTrain.hpp"
#include "hal/drivers/impl/linux/PwmOutput.hpp"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <vector>

namespace
{
    using namespace std::chrono_literals;
    class Pin final : public hal::IDigitalOutput
    {
      public:
        auto read() const noexcept -> hal::gpio::Level override { return m_level; }
        auto write(hal::gpio::Level level) noexcept -> void override { m_level = level; }
        auto toggle() noexcept -> void override { m_level = hal::gpio::inverted(m_level); }

      private:
        hal::gpio::Level m_level{ hal::gpio::Level::Low };
    };

    class PulseOutput : public testing::Test
    {
      protected:
        std::shared_ptr<Pin> pin{ std::make_shared<Pin>() };
        hal::PwmOutput output{ { hal::timer::Peripheral::Tim1, hal::timer::Channel::Channel1, { hal::gpio::Port::E, 9U } },
                               1U,
                               hal::detail::TimerLease{ hal::timer::Peripheral::Tim1 },
                               pin };

        void SetUp() override { ASSERT_TRUE(output.configure({ 100us, 5us })); }
    };
}

TEST_F(PulseOutput, EmitsExactlyOneOrManyCompletePulses)
{
    for (const auto target : { 1ULL, 2ULL, 65'537ULL }) {
        std::uint64_t last{};
        bool ordered{ true };
        ASSERT_TRUE(output.setPulseCallback([&](std::uint64_t count) noexcept {
            ordered &= count == last + 1U;
            ordered &= output.pulseCount() == count; // Callback queries must not deadlock.
            ordered &= output.isRunning();           // Last rising edge is not yet the final falling edge.
            last = count;
        }));
        ASSERT_TRUE(output.startPulses(target));
        EXPECT_EQ(output.pulseCount(), 0U);
        output.advanceSimulatedPulses(target - 1U);
        EXPECT_TRUE(output.isRunning());
        output.beginSimulatedPulse();
        EXPECT_EQ(output.pulseCount(), target);
        EXPECT_TRUE(output.isRunning()); // Do not shorten the final high phase.
        output.beginSimulatedPulse();    // Duplicate servicing cannot count twice.
        EXPECT_EQ(last, target);
        output.finishSimulatedPulse();
        EXPECT_FALSE(output.isRunning());
        output.advanceSimulatedPulses(10U);
        EXPECT_EQ(output.pulseCount(), target);
        EXPECT_EQ(last, target);
        EXPECT_TRUE(ordered);
        EXPECT_EQ(pin->read(), hal::gpio::Level::Low);
        ASSERT_TRUE(output.setPulseCallback({}));
    }
}

TEST_F(PulseOutput, RunsAnUnboundedCountedStreamUntilStopped)
{
    ASSERT_TRUE(output.startPulses());
    output.advanceSimulatedPulses(123U);
    EXPECT_TRUE(output.isRunning());
    EXPECT_EQ(output.pulseCount(), 123U);
    ASSERT_TRUE(output.stop());
    output.advanceSimulatedPulses(99U);
    EXPECT_FALSE(output.isRunning());
    EXPECT_EQ(output.pulseCount(), 123U);
    ASSERT_TRUE(output.configure({ 200us, 5us }));
    EXPECT_EQ(output.pulseCount(), 123U);
    ASSERT_TRUE(output.startPulses(1U));
    EXPECT_EQ(output.pulseCount(), 0U);
    output.advanceSimulatedPulses(100U);
    EXPECT_EQ(output.pulseCount(), 1U);
}

TEST_F(PulseOutput, AnInterruptedHighPhaseStillCountsItsRisingEdgeExactlyOnce)
{
    std::uint64_t callbacks{};
    ASSERT_TRUE(output.setPulseCallback([&](std::uint64_t) noexcept { ++callbacks; }));
    ASSERT_TRUE(output.startPulses(10U));
    output.advanceSimulatedPulses(2U);
    output.beginSimulatedPulse();
    ASSERT_TRUE(output.stop());
    ASSERT_TRUE(output.stop());
    output.finishSimulatedPulse();
    EXPECT_EQ(output.pulseCount(), 3U);
    EXPECT_EQ(callbacks, 3U);
    ASSERT_TRUE(output.startPulses(10U));
    ASSERT_TRUE(output.stop()); // Stop before the first rising edge.
    EXPECT_EQ(output.pulseCount(), 0U);
    EXPECT_EQ(callbacks, 3U);
}

TEST_F(PulseOutput, RejectsRunningMutationsWithoutReplacingTheTrainOrCallback)
{
    std::uint64_t callbacks{};
    ASSERT_TRUE(output.setPulseCallback([&](std::uint64_t) noexcept { ++callbacks; }));
    ASSERT_TRUE(output.startPulses(5U));
    output.advanceSimulatedPulses(2U);
    EXPECT_EQ(output.startPulses(1U).error(), std::errc::device_or_resource_busy);
    EXPECT_EQ(output.start().error(), std::errc::device_or_resource_busy);
    EXPECT_EQ(output.configure({ 200us, 5us }).error(), std::errc::device_or_resource_busy);
    EXPECT_EQ(output.setPulseCallback({}).error(), std::errc::device_or_resource_busy);
    EXPECT_EQ(output.pulseCount(), 2U);
    output.advanceSimulatedPulses(10U);
    EXPECT_EQ(output.pulseCount(), 5U);
    EXPECT_EQ(callbacks, 5U);
}

TEST_F(PulseOutput, RejectsDegeneratePulsesAndPreservesThePreviousCount)
{
    ASSERT_TRUE(output.startPulses(1U));
    output.advanceSimulatedPulses(1U);
    EXPECT_EQ(output.startPulses(0U).error(), std::errc::invalid_argument);
    EXPECT_EQ(output.pulseCount(), 1U);
    for (const auto high : { 0us, 100us }) {
        ASSERT_TRUE(output.configure({ 100us, high }));
        EXPECT_EQ(output.startPulses(1U).error(), std::errc::invalid_argument);
        EXPECT_FALSE(output.isRunning());
        EXPECT_EQ(output.pulseCount(), 1U);
        ASSERT_TRUE(output.start()); // Constant raw PWM is still supported.
        EXPECT_EQ(output.startPulses().error(), std::errc::device_or_resource_busy);
        EXPECT_EQ(output.pulseCount(), 0U);
        ASSERT_TRUE(output.stop());
        ASSERT_TRUE(output.configure({ 100us, 5us }));
        ASSERT_TRUE(output.startPulses(1U));
        output.advanceSimulatedPulses(1U);
    }
}

TEST_F(PulseOutput, ContinuousPwmIsUncountedAndDoesNotInvokeTheCallback)
{
    std::uint64_t callbacks{};
    ASSERT_TRUE(output.setPulseCallback([&](std::uint64_t) noexcept { ++callbacks; }));
    ASSERT_TRUE(output.start());
    ASSERT_TRUE(output.start());
    output.advanceSimulatedPulses(20U);
    EXPECT_TRUE(output.isRunning());
    EXPECT_EQ(output.pulseCount(), 0U);
    EXPECT_EQ(callbacks, 0U);
    ASSERT_TRUE(output.stop());
    ASSERT_TRUE(output.startPulses(1U));
    output.advanceSimulatedPulses(2U);
    EXPECT_EQ(callbacks, 1U);
}

TEST(PulseTrain, PendingEdgeQueriesDoNotConsumeItOrCountItTwice)
{
    hal::detail::PulseTrain train;
    train.start(1U);
    EXPECT_EQ(train.count(), 0U);
    EXPECT_EQ(train.count(true), 1U);
    EXPECT_EQ(train.count(true), 1U);
    EXPECT_TRUE(train.rise());
    EXPECT_FALSE(train.rise());
    EXPECT_EQ(train.count(true), 1U);
    EXPECT_FALSE(train.finishPulse());
    EXPECT_FALSE(train.rise());
    EXPECT_EQ(train.count(true), 1U);
}

TEST_F(PulseOutput, AcceptsA64BitTargetWithoutTruncatingItOrAllocatingAPulseBuffer)
{
    ASSERT_TRUE(output.startPulses(std::numeric_limits<std::uint64_t>::max()));
    output.advanceSimulatedPulses(65'537U);
    EXPECT_TRUE(output.isRunning());
    EXPECT_EQ(output.pulseCount(), 65'537U);
    ASSERT_TRUE(output.stop());
}

TEST(PulseTrain, RejectsUnconfiguredStartsAndDestructionDoesNotNotify)
{
    std::uint64_t callbacks{};
    {
        hal::PwmOutput output{ { hal::timer::Peripheral::Tim4, hal::timer::Channel::Channel3, { hal::gpio::Port::D, 14U } },
                               2U,
                               hal::detail::TimerLease{ hal::timer::Peripheral::Tim4 },
                               std::make_shared<Pin>() };
        EXPECT_EQ(output.startPulses(1U).error(), std::errc::operation_not_permitted);
        ASSERT_TRUE(output.configure({ 100us, 5us }));
        ASSERT_TRUE(output.setPulseCallback([&](std::uint64_t) noexcept { ++callbacks; }));
        ASSERT_TRUE(output.startPulses(10U));
        output.advanceSimulatedPulses(1U);
    }
    EXPECT_EQ(callbacks, 1U);
    EXPECT_TRUE(hal::detail::TimerLease{ hal::timer::Peripheral::Tim4 });
}
