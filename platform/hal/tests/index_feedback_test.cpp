#include "hal/board/board.hpp"
#include "hal/devices/impl/IndexFeedback.hpp"
#include "hal/drivers/impl/linux/Gpio.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <thread>

namespace
{
    using namespace std::chrono_literals;
    using namespace pnm::units::literals;
    using hal::device::IndexFeedback;
    using enum hal::gpio::Level;
    IndexFeedback::Clock::time_point current_time;
    auto now() noexcept -> IndexFeedback::Clock::time_point { return current_time; }

    class IndexFeedbackTest : public testing::Test
    {
      protected:
        std::shared_ptr<hal::IDigitalInput> pin{ hal::board::createStepperIndex(hal::board::MotorId::Motor2) };
        std::shared_ptr<hal::GpioInput> input{ std::dynamic_pointer_cast<hal::GpioInput>(pin) };
        IndexFeedback feedback{ pin, 1.8_deg, 16U, &now };
        IndexFeedback::Sample sample{};
        void SetUp() override
        {
            current_time = {};
            feedback.setCallback([this](const auto& value) noexcept { sample = value; });
            ASSERT_TRUE(feedback.start());
        }
        void TearDown() override { feedback.clearCallback(); }
        void cycleMotion(bool running, bool forward, std::chrono::nanoseconds period)
        {
            feedback.motion(running, forward, period / 64);
        }
        void edge(hal::gpio::Level level, std::chrono::milliseconds elapsed)
        {
            current_time += elapsed;
            input->setSimulatedLevel(level);
        }
    };
}

TEST_F(IndexFeedbackTest, WaitsForAnEdgeAndCountsCyclesInsteadOfCommandedSteps)
{
    EXPECT_EQ(sample.position.error(), std::errc::no_message_available);
    edge(High, 1ms); // Driver configuration/startup edges are not motion.
    edge(Low, 1ms);
    EXPECT_FALSE(sample.position);
    cycleMotion(true, true, 100ms);
    edge(High, 10ms);
    EXPECT_EQ(sample.position, 7.2_deg * 0.0);
    EXPECT_EQ((sample.velocity / (7.2_deg / 1_s)), 0);
    edge(Low, 10ms);
    edge(High, 90ms);
    EXPECT_EQ(sample.position, 7.2_deg * 1.0);
    EXPECT_DOUBLE_EQ((sample.velocity / (7.2_deg / 1_s)), 10);
    cycleMotion(false, true, 100ms);
    EXPECT_EQ(sample.position, 7.2_deg * 1.0);
    EXPECT_EQ((sample.velocity / (7.2_deg / 1_s)), 0);
}

TEST_F(IndexFeedbackTest, ReversalsCrossingTheSameBoundaryCancelWithoutDrift)
{
    cycleMotion(true, true, 100ms);
    edge(High, 100ms);
    for (int i = 0; i < 5; ++i) {
        cycleMotion(false, true, 100ms);
        cycleMotion(true, false, 100ms);
        edge(Low, 1ms); // Undo the same boundary, NOT another electrical period.
        EXPECT_EQ(sample.position, 7.2_deg * -1.0);
        EXPECT_EQ((sample.velocity / (7.2_deg / 1_s)), 0);
        cycleMotion(false, false, 100ms);
        cycleMotion(true, true, 100ms);
        edge(High, 1ms);
        EXPECT_EQ(sample.position, 7.2_deg * 0.0);
        EXPECT_EQ((sample.velocity / (7.2_deg / 1_s)), 0);
    }
}

TEST_F(IndexFeedbackTest, BackwardVelocityUsesFallingEdgesAndStopSettlingHasZeroVelocity)
{
    cycleMotion(true, false, 100ms);
    edge(High, 10ms);
    edge(Low, 10ms);
    edge(High, 90ms);
    edge(Low, 10ms);
    EXPECT_EQ(sample.position, 7.2_deg * -1.0);
    EXPECT_DOUBLE_EQ((sample.velocity / (7.2_deg / 1_s)), -10);
    cycleMotion(false, false, 100ms);
    edge(High, 90ms);
    edge(Low, 10ms); // Final interpolation may finish after STEP completion.
    EXPECT_EQ(sample.position, 7.2_deg * -2.0);
    EXPECT_EQ((sample.velocity / (7.2_deg / 1_s)), 0);
    cycleMotion(true, false, 100ms);
    edge(High, 90ms);
    edge(Low, 10ms);
    EXPECT_EQ(sample.position, 7.2_deg * -3.0);
    EXPECT_EQ((sample.velocity / (7.2_deg / 1_s)), 0); // No rest time in a velocity interval.
}

TEST_F(IndexFeedbackTest, MissingEdgesLatchInvalidFeedbackUntilReferenceOrReset)
{
    cycleMotion(true, true, 100ms);
    edge(High, 10ms);
    current_time += 301ms;
    cycleMotion(true, true, 100ms);
    EXPECT_EQ(sample.position.error(), std::errc::state_not_recoverable);
    EXPECT_EQ((sample.velocity / (7.2_deg / 1_s)), 0);
    edge(Low, 10ms);
    edge(High, 90ms);
    EXPECT_FALSE(sample.position); // Unknown missed cycles must not silently recover.
    cycleMotion(false, true, 100ms);
    ASSERT_TRUE(feedback.reference(0_deg));
    EXPECT_EQ(sample.position, 7.2_deg * 0.0);
    feedback.invalidate();
    EXPECT_EQ(sample.position.error(), std::errc::no_message_available);
    edge(Low, 10ms);
    edge(High, 90ms);
    EXPECT_FALSE(sample.position);
}

TEST_F(IndexFeedbackTest, ReferenceDoesNotInventAConnectedSensor)
{
    ASSERT_TRUE(feedback.reference(0_deg));
    EXPECT_EQ(sample.position.error(), std::errc::no_message_available);
    cycleMotion(true, true, 100ms);
    edge(High, 100ms);
    edge(Low, 10ms);
    edge(High, 90ms);
    EXPECT_EQ(sample.position, 7.2_deg * 1.0);
    cycleMotion(false, true, 100ms);
    ASSERT_TRUE(feedback.reference(0_deg));
    EXPECT_EQ(sample.position, 7.2_deg * 0.0);
    EXPECT_EQ((sample.velocity / (7.2_deg / 1_s)), 0);
}

TEST_F(IndexFeedbackTest, FasterCommandDoesNotMisclassifyThePreviousSlowInterval)
{
    cycleMotion(true, true, 1s);
    edge(High, 10ms);
    current_time += 500ms;
    cycleMotion(true, true, 10ms);
    EXPECT_TRUE(sample.position);
    edge(Low, 1ms);
    edge(High, 9ms);
    EXPECT_EQ(sample.position, 7.2_deg * 1.0);
    current_time += 31ms;
    cycleMotion(true, true, 10ms);
    EXPECT_FALSE(sample.position); // Now the new, fast interval really is overdue.
}

TEST_F(IndexFeedbackTest, SameTimestampCrossingsAreAccumulatedForVelocity)
{
    cycleMotion(true, true, 500us);
    edge(High, 0ms);
    for (unsigned tick = 0; tick < 10; ++tick) {
        edge(Low, 0ms);
        edge(High, 0ms); // Two cycles within one tick, as at 128 kSTEP/s.
        edge(Low, 0ms);
        edge(High, 1ms);
    }
    EXPECT_EQ(sample.position, 7.2_deg * 20.0);
    EXPECT_DOUBLE_EQ((sample.velocity / (7.2_deg / 1_s)), 2000);
}

TEST(IndexFeedback, ConfiguredMicrostepsConvertStepPeriodIntoTheFreshnessDeadline)
{
    current_time = {};
    auto pin{ hal::board::createStepperIndex(hal::board::MotorId::Motor3) };
    auto input{ std::dynamic_pointer_cast<hal::GpioInput>(pin) };
    IndexFeedback::Sample sample;
    IndexFeedback feedback{ pin, 0.9_deg, 32U, &now };
    feedback.setCallback([&](const auto& value) noexcept { sample = value; });
    feedback.motion(true, true, 1ms);
    input->setSimulatedLevel(High);
    EXPECT_FALSE(sample.position); // Construction does not start observation.
    input->setSimulatedLevel(Low);
    ASSERT_TRUE(feedback.start());
    feedback.motion(true, true, 1ms);
    input->setSimulatedLevel(High);
    ASSERT_TRUE(sample.position);
    EXPECT_EQ(feedback.reference(10_deg).error(), std::errc::device_or_resource_busy);
    current_time += 200ms; // Beyond the 1/16 deadline, within 3 * 128 ms at 1/32.
    feedback.motion(true, true, 1ms);
    EXPECT_TRUE(sample.position);
    current_time += 185ms;
    feedback.motion(true, true, 1ms);
    EXPECT_EQ(sample.position.error(), std::errc::state_not_recoverable);
    EXPECT_EQ(sample.velocity, 0_rpm);
    EXPECT_FALSE(sample.reference_lost); // Missing INDEX does not lose a switch datum.
}

TEST(IndexFeedback, DestructionDisconnectsAndWaitsForConcurrentCallbacks)
{
    auto pin{ hal::board::createStepperIndex(hal::board::MotorId::Motor3) };
    auto input{ std::dynamic_pointer_cast<hal::GpioInput>(pin) };
    auto feedback{ std::make_unique<IndexFeedback>(pin, 1.8_deg, 16U) };
    std::atomic<unsigned> callbacks{};
    feedback->setCallback([&](const auto&) noexcept { callbacks.fetch_add(1); });
    ASSERT_TRUE(feedback->start());
    feedback->motion(true, true, 1562500ns);
    std::jthread edges{ [&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            input->setSimulatedLevel(High);
            input->setSimulatedLevel(Low);
        }
    } };
    std::this_thread::sleep_for(10ms);
    feedback.reset();
    const auto after{ callbacks.load() };
    std::this_thread::sleep_for(10ms);
    EXPECT_EQ(callbacks.load(), after);
}
