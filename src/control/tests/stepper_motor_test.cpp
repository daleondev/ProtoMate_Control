#include "../StepperMotor.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace
{
    using enum hal::board::MotorId;
    using enum StepperMotor::Result;
    using namespace std::chrono_literals;
    using namespace pnm::units::literals;

    bool running(const std::shared_ptr<hal::IStepOutput>& axis)
    {
        const auto deadline{ std::chrono::steady_clock::now() + 500ms };
        while (std::chrono::steady_clock::now() < deadline) {
            if (axis->status().state == hal::step::State::Running)
                return true;
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }
    StepperMotor::Result result(std::future<StepperMotor::Result>& motion)
    {
        EXPECT_EQ(motion.wait_for(2s), std::future_status::ready);
        return motion.get();
    }
}

TEST(StepperMotor, RequiresExternalGeneratorStartAndTracksRelativeAndAbsolutePosition)
{
    const auto generator{ hal::board::createStepperGenerator() };
    ASSERT_NE(generator, nullptr);
    StepperMotor motor{ Motor2, 1.8_deg, 16U, generator };
    const auto axis{ generator->output(hal::step::Axis::_2) };
    auto rejected{ motor.moveRel(90_deg, 300_rpm) };
    EXPECT_EQ(result(rejected), Rejected);
    EXPECT_EQ(*axis->pulseCount(), 0U);
    ASSERT_TRUE(generator->start());
    auto forward{ motor.moveRel(90_deg, 300_rpm) };
    EXPECT_EQ(result(forward), Completed);
    EXPECT_EQ(*axis->pulseCount(), 800U);
    EXPECT_NEAR(motor.position().get<pnm::units::AngleUnits::deg>(), 90.0, 1e-9);
    auto backward{ motor.moveRel(-45_deg, 300_rpm) };
    EXPECT_EQ(result(backward), Completed);
    EXPECT_EQ(*axis->pulseCount(), 400U);
    auto absolute{ motor.moveAbs(0_deg, 300_rpm) };
    EXPECT_EQ(result(absolute), Completed);
    EXPECT_EQ(*axis->pulseCount(), 400U);
    EXPECT_NEAR(motor.position().get<pnm::units::AngleUnits::deg>(), 0.0, 1e-9);
    EXPECT_EQ(generator->status().state, hal::step::State::Running);
}

TEST(StepperMotor, IndependentStopRestartVelocityAndTimeoutLeaveOtherMotorRunning)
{
    const auto generator{ hal::board::createStepperGenerator() };
    ASSERT_NE(generator, nullptr);
    StepperMotor first{ Motor2, 1.8_deg, 16U, generator };
    StepperMotor second{ Motor3, 1.8_deg, 16U, generator };
    const auto axis{ generator->output(hal::step::Axis::_2) };
    const auto other{ generator->output(hal::step::Axis::_3) };
    ASSERT_TRUE(generator->start());
    auto continuous{ first.move(StepperMotor::Direction::Forward, 300_rpm) };
    ASSERT_TRUE(running(axis));
    auto replacement{ second.move(StepperMotor::Direction::Backward, 300_rpm) };
    ASSERT_TRUE(running(other));
    auto updated{ first.setVelocity(600_rpm) };
    ASSERT_TRUE(updated);
    EXPECT_GT(*updated, 0U);
    EXPECT_FALSE(first.setVelocity(0_rpm));
    const auto before{ *axis->pulseCount() };
    second.stopAndWait();
    EXPECT_EQ(result(replacement), Stopped);
    const auto origin{ second.position() };
    auto finite{ second.moveRel(45_deg, 300_rpm) };
    EXPECT_EQ(result(finite), Completed);
    EXPECT_EQ(*other->pulseCount(), 400U);
    EXPECT_NEAR((second.position() - origin).get<pnm::units::AngleUnits::deg>(), 45.0, 1e-9);
    auto timeout{ second.move(StepperMotor::Direction::Forward, 300_rpm, 0.01_s) };
    EXPECT_EQ(result(timeout), TimedOut);
    EXPECT_EQ(axis->status().state, hal::step::State::Running);
    EXPECT_GT(*axis->pulseCount(), before);
    first.stopAndWait();
    EXPECT_EQ(result(continuous), Stopped);
    EXPECT_EQ(generator->status().state, hal::step::State::Running);
}

TEST(StepperMotor, ReplacingMoveJoinsAndAccountsPreviousMotionBeforeRelativeTarget)
{
    const auto generator{ hal::board::createStepperGenerator() };
    ASSERT_NE(generator, nullptr);
    StepperMotor motor{ Motor3, 1.8_deg, 16U, generator };
    ASSERT_TRUE(generator->start());
    const auto axis{ generator->output(hal::step::Axis::_3) };
    auto old{ motor.move(StepperMotor::Direction::Forward, 300_rpm) };
    ASSERT_TRUE(running(axis));
    std::this_thread::sleep_for(10ms);
    auto next{ motor.moveRel(45_deg, 300_rpm) };
    EXPECT_EQ(result(old), Stopped);
    EXPECT_EQ(result(next), Completed);
    EXPECT_EQ(*axis->pulseCount(), 400U);
    EXPECT_GT(motor.position().get<pnm::units::AngleUnits::deg>(), 45.0);
}

TEST(StepperMotor, InvalidRequestsAndDestructionDoNotStopOtherAxis)
{
    const auto generator{ hal::board::createStepperGenerator() };
    ASSERT_NE(generator, nullptr);
    EXPECT_THROW((StepperMotor{ Motor3, 1.8_deg, 0U, generator }), std::invalid_argument);
    StepperMotor first{ Motor2, 1.8_deg, 16U, generator };
    ASSERT_TRUE(generator->start());
    const auto axis{ generator->output(hal::step::Axis::_2) };
    auto continuous{ first.move(StepperMotor::Direction::Forward, 300_rpm) };
    ASSERT_TRUE(running(axis));
    std::future<StepperMotor::Result> destroyed;
    {
        StepperMotor second{ Motor3, 1.8_deg, 16U, generator };
        auto bad{ second.moveRel(90_deg, 0_rpm) };
        EXPECT_EQ(result(bad), Rejected);
        auto negative{ second.moveRel(90_deg, -1_rpm) };
        EXPECT_EQ(result(negative), Rejected);
        auto nan{ second.moveAbs(1_deg * std::numeric_limits<double>::quiet_NaN(), 300_rpm) };
        EXPECT_EQ(result(nan), Rejected);
        destroyed = second.move(StepperMotor::Direction::Forward, 300_rpm);
        ASSERT_TRUE(running(generator->output(hal::step::Axis::_3)));
    }
    EXPECT_EQ(result(destroyed), Stopped);
    EXPECT_EQ(axis->status().state, hal::step::State::Running);
    first.stopAndWait();
    EXPECT_EQ(result(continuous), Stopped);
}

TEST(StepperMotor, CompletionAndCancellationWakeBlockedWorkersAcrossManyReplacements)
{
    const auto generator{ hal::board::createStepperGenerator() };
    ASSERT_NE(generator, nullptr);
    StepperMotor motor{ Motor2, 1.8_deg, 16U, generator };
    ASSERT_TRUE(generator->start());
    for (unsigned i = 0; i < 30; ++i) {
        auto previous{ motor.move(StepperMotor::Direction::Forward, 300_rpm) };
        // Replace even when the prior worker has not reached its wait/start yet.
        auto next{ motor.moveRel(0.1125_deg, 300_rpm) };
        EXPECT_EQ(result(previous), Stopped);
        EXPECT_EQ(result(next), Completed);
    }
    auto moving{ motor.move(StepperMotor::Direction::Forward, 300_rpm) };
    ASSERT_TRUE(running(generator->output(hal::step::Axis::_2)));
    std::this_thread::sleep_for(10ms);
    const auto before{ motor.position() };
    std::this_thread::sleep_for(10ms);
    EXPECT_GT(motor.position(), before); // Position refreshes without waking the motion worker.
    motor.stopAndWait();
    EXPECT_EQ(result(moving), Stopped);
}

TEST(StepperMotor, GlobalStopWakesAllMotorsAndEachCanBeUsedAfterAnExplicitRestart)
{
    const auto generator{ hal::board::createStepperGenerator() };
    ASSERT_NE(generator, nullptr);
    StepperMotor first{ Motor2, 1.8_deg, 16U, generator };
    StepperMotor second{ Motor3, 1.8_deg, 16U, generator };
    ASSERT_TRUE(generator->start());
    auto a{ first.move(StepperMotor::Direction::Forward, 300_rpm) };
    auto b{ second.move(StepperMotor::Direction::Forward, 300_rpm) };
    ASSERT_TRUE(running(generator->output(hal::step::Axis::_2)));
    ASSERT_TRUE(running(generator->output(hal::step::Axis::_3)));
    static_cast<void>(generator->stop());
    EXPECT_EQ(result(a), Stopped);
    EXPECT_EQ(result(b), Stopped);
    ASSERT_TRUE(generator->start());
    auto next{ first.moveRel(1.125_deg, 300_rpm) };
    EXPECT_EQ(result(next), Completed);
}
