#include "hal/board/board.hpp"
#include "hal/devices/impl/IndexFeedback.hpp"
#include "hal/drivers/impl/linux/Gpio.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <thread>

namespace
{
    using namespace std::chrono_literals;
    using hal::device::IndexFeedback;
    using enum hal::gpio::Level;
    IndexFeedback::Clock::time_point current_time;
    auto now() noexcept -> IndexFeedback::Clock::time_point { return current_time; }

    class IndexFeedbackTest : public testing::Test
    {
      protected:
        std::shared_ptr<hal::IDigitalInput> pin{ hal::board::createStepperIndex(hal::board::MotorId::Motor2) };
        std::shared_ptr<hal::GpioInput> input{ std::dynamic_pointer_cast<hal::GpioInput>(pin) };
        IndexFeedback feedback{ pin, &now };
        IndexFeedback::Sample sample{};
        void SetUp() override
        {
            current_time = {};
            feedback.setCallback([this](const auto& value) noexcept { sample = value; });
        }
        void TearDown() override { feedback.clearCallback(); }
        void edge(hal::gpio::Level level, std::chrono::milliseconds elapsed)
        {
            current_time += elapsed;
            input->setSimulatedLevel(level);
        }
    };
}

TEST_F(IndexFeedbackTest, WaitsForAnEdgeAndCountsCyclesInsteadOfCommandedSteps)
{
    EXPECT_EQ(sample.cycles.error(), std::errc::no_message_available);
    edge(High, 1ms); // Driver configuration/startup edges are not motion.
    edge(Low, 1ms);
    EXPECT_FALSE(sample.cycles);
    feedback.motion(true, true, 100ms);
    edge(High, 10ms);
    EXPECT_EQ(sample.cycles, 0);
    EXPECT_EQ(sample.cycles_per_second, 0);
    edge(Low, 10ms);
    edge(High, 90ms);
    EXPECT_EQ(sample.cycles, 1);
    EXPECT_DOUBLE_EQ(sample.cycles_per_second, 10);
    feedback.motion(false, true, 100ms);
    EXPECT_EQ(sample.cycles, 1);
    EXPECT_EQ(sample.cycles_per_second, 0);
}

TEST_F(IndexFeedbackTest, ReversalsCrossingTheSameBoundaryCancelWithoutDrift)
{
    feedback.motion(true, true, 100ms);
    edge(High, 100ms);
    for (int i = 0; i < 5; ++i) {
        feedback.motion(false, true, 100ms);
        feedback.motion(true, false, 100ms);
        edge(Low, 1ms); // Undo the same boundary, NOT another electrical period.
        EXPECT_EQ(sample.cycles, -1);
        EXPECT_EQ(sample.cycles_per_second, 0);
        feedback.motion(false, false, 100ms);
        feedback.motion(true, true, 100ms);
        edge(High, 1ms);
        EXPECT_EQ(sample.cycles, 0);
        EXPECT_EQ(sample.cycles_per_second, 0);
    }
}

TEST_F(IndexFeedbackTest, BackwardVelocityUsesFallingEdgesAndStopSettlingHasZeroVelocity)
{
    feedback.motion(true, false, 100ms);
    edge(High, 10ms);
    edge(Low, 10ms);
    edge(High, 90ms);
    edge(Low, 10ms);
    EXPECT_EQ(sample.cycles, -1);
    EXPECT_DOUBLE_EQ(sample.cycles_per_second, -10);
    feedback.motion(false, false, 100ms);
    edge(High, 90ms);
    edge(Low, 10ms); // Final interpolation may finish after STEP completion.
    EXPECT_EQ(sample.cycles, -2);
    EXPECT_EQ(sample.cycles_per_second, 0);
    feedback.motion(true, false, 100ms);
    edge(High, 90ms);
    edge(Low, 10ms);
    EXPECT_EQ(sample.cycles, -3);
    EXPECT_EQ(sample.cycles_per_second, 0); // No rest time in a velocity interval.
}

TEST_F(IndexFeedbackTest, MissingEdgesLatchInvalidFeedbackUntilReferenceOrReset)
{
    feedback.motion(true, true, 100ms);
    edge(High, 10ms);
    current_time += 301ms;
    feedback.motion(true, true, 100ms);
    EXPECT_EQ(sample.cycles.error(), std::errc::state_not_recoverable);
    EXPECT_EQ(sample.cycles_per_second, 0);
    edge(Low, 10ms);
    edge(High, 90ms);
    EXPECT_FALSE(sample.cycles); // Unknown missed cycles must not silently recover.
    feedback.motion(false, true, 100ms);
    feedback.reference();
    EXPECT_EQ(sample.cycles, 0);
    feedback.invalidate();
    EXPECT_EQ(sample.cycles.error(), std::errc::no_message_available);
    edge(Low, 10ms);
    edge(High, 90ms);
    EXPECT_FALSE(sample.cycles);
}

TEST_F(IndexFeedbackTest, ReferenceDoesNotInventAConnectedSensor)
{
    feedback.reference();
    EXPECT_EQ(sample.cycles.error(), std::errc::no_message_available);
    feedback.motion(true, true, 100ms);
    edge(High, 100ms);
    edge(Low, 10ms);
    edge(High, 90ms);
    EXPECT_EQ(sample.cycles, 1);
    feedback.motion(false, true, 100ms);
    feedback.reference();
    EXPECT_EQ(sample.cycles, 0);
    EXPECT_EQ(sample.cycles_per_second, 0);
}

TEST_F(IndexFeedbackTest, FasterCommandDoesNotMisclassifyThePreviousSlowInterval)
{
    feedback.motion(true, true, 1s);
    edge(High, 10ms);
    current_time += 500ms;
    feedback.motion(true, true, 10ms);
    EXPECT_TRUE(sample.cycles);
    edge(Low, 1ms);
    edge(High, 9ms);
    EXPECT_EQ(sample.cycles, 1);
    current_time += 31ms;
    feedback.motion(true, true, 10ms);
    EXPECT_FALSE(sample.cycles); // Now the new, fast interval really is overdue.
}

TEST_F(IndexFeedbackTest, SameTimestampCrossingsAreAccumulatedForVelocity)
{
    feedback.motion(true, true, 500us);
    edge(High, 0ms);
    for (unsigned tick = 0; tick < 10; ++tick) {
        edge(Low, 0ms);
        edge(High, 0ms); // Two cycles within one tick, as at 128 kSTEP/s.
        edge(Low, 0ms);
        edge(High, 1ms);
    }
    EXPECT_EQ(sample.cycles, 20);
    EXPECT_DOUBLE_EQ(sample.cycles_per_second, 2000);
}

TEST(IndexFeedback, DestructionDisconnectsAndWaitsForConcurrentCallbacks)
{
    auto pin{ hal::board::createStepperIndex(hal::board::MotorId::Motor3) };
    auto input{ std::dynamic_pointer_cast<hal::GpioInput>(pin) };
    auto feedback{ std::make_unique<IndexFeedback>(pin) };
    std::atomic<unsigned> callbacks{};
    feedback->setCallback([&](const auto&) noexcept { callbacks.fetch_add(1); });
    feedback->motion(true, true, 100ms);
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
