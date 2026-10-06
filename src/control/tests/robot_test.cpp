#include "control/Robot.hpp"
#include "control/SynchronizedSequence.hpp"
#include "hal/drivers/impl/linux/Gpio.hpp"
#include "hal/drivers/impl/linux/QuadratureEncoder.hpp"
#include "hal/drivers/detail/SimulatedStepHardware.hpp"
#include "hal/drivers/detail/StepGenerator.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace
{
    using Controller = control::MotionController;
    using Kinematics = ScaraKinematics;
    using enum Controller::MotorId;
    using namespace pnm::units;
    using namespace pnm::units::literals;
    using namespace std::chrono_literals;

    const Kinematics::Config geometry{ .first_arm_length = 200_mm, .second_arm_length = 100_mm };

    class RobotState : public testing::Test
    {
      protected:
        std::shared_ptr<Controller> controller;

        void SetUp() override
        {
            controller = std::make_shared<Controller>(std::array<Controller::AxisConfig, 3>{ {
              { 135_deg, 1.8_deg, 16U },
              { 135_deg, 1.8_deg, 16U },
              { 135_deg, 1.8_deg, 16U },
            } });
            auto alarm = hal::gpio::simulatedInput({ hal::gpio::Port::F, 2 });
            ASSERT_TRUE(alarm);
            alarm->setSimulatedLevel(hal::gpio::Level::Low); // Healthy, powered DM542T.
            controller->initializeDrivers();
            for (const auto pin : { 7U, 8U, 10U }) {
                auto input{ hal::gpio::simulatedInput(
                  { hal::gpio::Port::E, static_cast<std::uint8_t>(pin) }) };
                ASSERT_TRUE(input);
                input->setSimulatedLevel(hal::gpio::Level::Low);
            }
        }

        void configure()
        {
            // Zero motor angles map to joint coordinates (0 deg, 90 deg, 10 mm).
            // Non-unit reductions and inverted shoulder/Z exercise all conversions.
            controller->configureAxis(Motor1,
                                      RotaryAxisConversion{ {
                                        .motor_revolutions_per_axis_revolution = 2,
                                        .direction = AxisDirection::OppositeToMotor,
                                        .motor_reference = 135_deg,
                                        .axis_reference = -67.5_deg,
                                      } });
            controller->configureAxis(Motor2,
                                      RotaryAxisConversion{ {
                                        .motor_revolutions_per_axis_revolution = 3,
                                        .motor_reference = 135_deg,
                                        .axis_reference = 135_deg,
                                      } });
            controller->configureAxis(Motor3,
                                      LinearAxisConversion{ {
                                        .travel_per_output_revolution = 40_mm,
                                        .motor_revolutions_per_output_revolution = 2,
                                        .direction = AxisDirection::OppositeToMotor,
                                        .motor_reference = 135_deg,
                                        .axis_reference = 2.5_mm,
                                      } });
        }

        bool home(control::Robot& robot)
        {
            robot.enable();
            auto operation{ robot.reference(5_s) };
            constexpr std::array pins{ 7U, 8U, 10U };
            for (const auto index : { 2U, 0U, 1U }) {
                const auto input{ hal::gpio::simulatedInput(
                  { hal::gpio::Port::E, static_cast<std::uint8_t>(pins[index]) }) };
                if (!eventually([&] { return controller->status().axes[index].velocity > 0_rpm; }))
                    return false;
                input->setSimulatedLevel(hal::gpio::Level::High);
                if (!eventually([&] { return controller->status().axes[index].velocity < 0_rpm; }))
                    return false;
                input->setSimulatedLevel(hal::gpio::Level::Low);
                if (!eventually([&] { return controller->status().axes[index].velocity > 0_rpm; }))
                    return false;
                input->setSimulatedLevel(hal::gpio::Level::High);
                if (!eventually([&] { return controller->status().axes[index].referenced; }))
                    return false;
                input->setSimulatedLevel(hal::gpio::Level::Low);
            }
            return operation.completion.wait_for(2s) == std::future_status::ready &&
                   operation.completion.get() == StepperMotor::Result::Completed;
        }

        template<typename Predicate>
        bool eventually(Predicate predicate)
        {
            const auto deadline{ std::chrono::steady_clock::now() + 1s };
            do {
                if (predicate())
                    return true;
                std::this_thread::sleep_for(1ms);
            } while (std::chrono::steady_clock::now() < deadline);
            return false;
        }
    };
}

TEST(Robot, RejectsMissingController)
{
    EXPECT_THROW((control::Robot{ nullptr, geometry }), std::invalid_argument);
}

TEST_F(RobotState, RequiresMechanicsForLiveStateButNotForGeometryCalculations)
{
    const control::Robot robot{ controller, geometry };
    EXPECT_THROW(robot.status(), std::runtime_error);
    ASSERT_TRUE(robot.kinematics().forward({ 0_deg, 90_deg, 10_mm }));
    configure();
    EXPECT_NO_THROW(robot.status());
}

TEST_F(RobotState, ConvertsOffsetsReductionsAndDirectionsBeforeComputingToolState)
{
    configure();
    const control::Robot robot{ controller, geometry };
    const auto status{ robot.status() };
    EXPECT_FALSE(status.motors.enabled);
    EXPECT_FALSE(status.referenced);
    EXPECT_TRUE(status.motors.generator.counts_exact);
    const auto& state{ status.commanded };
    EXPECT_NEAR(state.joints.shoulder / 1_deg, 0, 1e-9);
    EXPECT_NEAR(state.joints.elbow / 1_deg, 90, 1e-9);
    EXPECT_NEAR(state.joints.z / 1_mm, 10, 1e-9);
    ASSERT_TRUE(state.pose);
    EXPECT_NEAR(state.pose->position.x / 1_mm, 200, 1e-9);
    EXPECT_NEAR(state.pose->position.y / 1_mm, 100, 1e-9);
    EXPECT_NEAR(state.pose->position.z / 1_mm, 10, 1e-9);
    EXPECT_NEAR(state.pose->yaw / 1_deg, 90, 1e-9);
    ASSERT_TRUE(state.velocity);
    EXPECT_EQ(state.velocity->linear.x, 0_mm_s);
    EXPECT_EQ(state.velocity->linear.y, 0_mm_s);
    EXPECT_EQ(state.velocity->linear.z, 0_mm_s);
    EXPECT_EQ(state.velocity->yaw, 0_rpm);
    ASSERT_FALSE(status.actual);
    EXPECT_EQ(status.actual.error().motor, Motor2);
    EXPECT_EQ(status.actual.error().reason, std::errc::no_message_available);
    EXPECT_TRUE(controller->motions().empty());
}

TEST_F(RobotState, LiveVelocityAndPositionTrackSharedMotorMotionAndStop)
{
    configure();
    const control::Robot robot{ controller, geometry };
    controller->enable();
    auto move{ controller->move(Motor3, { .position = -360_deg, .velocity = 30_rpm }) };
    ASSERT_TRUE(eventually([&] { return robot.status().commanded.joint_velocity.z > 0_mm_s; }));
    const auto active{ robot.status() };
    const auto& state{ active.commanded };
    EXPECT_GT(state.joint_velocity.z, 0_mm_s);
    EXPECT_LE(active.motors.axes[2].velocity, 0_rpm);
    // 20 mm/revolution, inverted direction: 1 motor rpm gives -1/3 mm/s.
    EXPECT_NEAR(state.joint_velocity.z / 1_mm_s, -active.motors.axes[2].velocity / 1_rpm / 3, 1e-9);
    ASSERT_TRUE(state.velocity);
    EXPECT_EQ(state.velocity->linear.z, state.joint_velocity.z);
    EXPECT_EQ(state.velocity->linear.x, 0_mm_s);
    EXPECT_EQ(state.velocity->linear.y, 0_mm_s);
    controller->stop(Motor3);
    ASSERT_EQ(move.completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(move.completion.get(), StepperMotor::Result::Stopped);
    const auto stopped{ robot.status() };
    EXPECT_GT(stopped.commanded.joints.z, 10_mm);
    EXPECT_EQ(stopped.commanded.joint_velocity.z, 0_mm_s);
    ASSERT_TRUE(stopped.commanded.pose);
    EXPECT_EQ(stopped.commanded.pose->position.z, stopped.commanded.joints.z);
    EXPECT_NEAR(stopped.commanded.joints.z / 1_mm, 10 - stopped.motors.axes[2].position / 1_deg / 18, 1e-9);
}

TEST_F(RobotState, FeedbackRemainsDistinctFromCommandedStateAndPropagatesFaults)
{
    configure();
    const control::Robot robot{ controller, geometry };
    auto encoder{ hal::encoder::simulatedEncoder(3U) };
    ASSERT_TRUE(encoder);
    ASSERT_TRUE(encoder->advanceSimulatedCounts(400));
    ASSERT_TRUE(eventually([&] {
        const auto value{ robot.status().motors.axes[0].actual_position };
        return value && std::abs(*value / 1_deg - 90) < 1e-8;
    }));
    const auto state{ robot.status() };
    EXPECT_NEAR(state.commanded.joints.shoulder / 1_deg, 0, 1e-9);
    ASSERT_FALSE(state.actual);
    EXPECT_EQ(state.actual.error().motor, Motor2);
    EXPECT_EQ(state.actual.error().reason, std::errc::no_message_available);
    ASSERT_TRUE(encoder->stop());
    ASSERT_TRUE(encoder->setPosition(std::numeric_limits<std::int64_t>::max()));
    ASSERT_TRUE(encoder->start());
    EXPECT_FALSE(encoder->advanceSimulatedCounts(1));
    ASSERT_TRUE(eventually([&] {
        const auto status{ robot.status() };
        return !status.actual && status.actual.error().motor == Motor1;
    }));
    const auto faulted{ robot.status() };
    EXPECT_NE(faulted.actual.error().reason, std::errc::no_message_available);
    EXPECT_TRUE(faulted.commanded.pose);
}

TEST_F(RobotState, ReconfigurationIsVisibleWithoutRecreatingRobot)
{
    configure();
    const control::Robot robot{ controller, geometry };
    const auto before{ robot.status() };
    controller->configureAxis(Motor3,
                              LinearAxisConversion{ {
                                .travel_per_output_revolution = 40_mm,
                                .motor_reference = 135_deg,
                                .axis_reference = 100_mm,
                              } });
    const auto after{ robot.status() };
    EXPECT_NEAR(before.commanded.joints.z / 1_mm, 10, 1e-9);
    EXPECT_NEAR(after.commanded.joints.z / 1_mm, 85, 1e-9);
    ASSERT_TRUE(after.commanded.pose);
    EXPECT_NEAR(after.commanded.pose->position.z / 1_mm, 85, 1e-9);
}

TEST(SynchronizedSequence, UnequalCountsShareTheFinalTickWithoutAccumulatedRounding)
{
    auto profile{ motion::Profile::create(1.0, { 0.73, 1.1, 0.6, 4.2 }) };
    ASSERT_TRUE(profile);
    std::optional<std::chrono::nanoseconds> final;
    for (const auto count : { 1U, 7U, 641U, 1753U }) {
        motion::SynchronizedSequence sequence{ *profile, count };
        auto time{ sequence.firstDelay() };
        for (unsigned begin{}; begin < count;) {
            std::array<hal::step::Timing, 137> batch;
            auto block{ std::span{ batch }.first(std::min<std::size_t>(batch.size(), count - begin)) };
            ASSERT_TRUE(sequence.generate(begin, block));
            for (const auto timing : block) {
                ASSERT_TRUE(sequence.timing(begin));
                EXPECT_EQ(timing, *sequence.timing(begin));
                EXPECT_GE(timing.period, 10us);
                EXPECT_LE(timing.period, sequence.maximumPeriod());
                EXPECT_EQ(timing.high_time, 5us);
                if (++begin < count)
                    time += timing.period;
            }
        }
        if (final)
            EXPECT_EQ(time, *final);
        final = time;
        EXPECT_FALSE(sequence.timing(count));
    }
}

TEST(SynchronizedSequence, SharedProfileProducesExactEdgesThroughDmaRefillsAndRollover)
{
    auto port{ std::make_unique<hal::detail::SimulatedStepHardware>() };
    auto* hardware{ port.get() };
    auto generator{ std::make_shared<hal::detail::StepGenerator>(std::move(port)) };
    hardware->interrupt = [&] { generator->service(); };
    const auto profile{ motion::Profile::create(1.0, { 0.73, 1.1, 0.6, 4.2 }) };
    ASSERT_TRUE(profile);
    constexpr std::array counts{ 7U, 641U, 1753U };
    std::array<std::shared_ptr<motion::SynchronizedSequence>, 3> sequences;
    std::array<std::optional<std::chrono::nanoseconds>, 3> delays;
    ASSERT_TRUE(generator->start());
    hardware->advance(hal::detail::step_park - 1000U);
    const auto origin{ hardware->elapsed };
    for (unsigned i{}; i < 3U; ++i) {
        sequences[i] = std::make_shared<motion::SynchronizedSequence>(*profile, counts[i]);
        ASSERT_TRUE(generator->output(static_cast<hal::step::Axis>(i))->prepareSequence(sequences[i]));
        delays[i] = sequences[i]->firstDelay() + 10ms;
    }
    ASSERT_TRUE(generator->startPrepared(delays));
    hardware->advance(static_cast<std::uint64_t>(std::ceil(profile->duration() * 1e7)) + 200'000U);
    const auto final{ origin + 100'000U + static_cast<std::uint64_t>(std::ceil(profile->duration() * 1e7)) };
    for (unsigned axis{}; axis < 3U; ++axis) {
        unsigned pulse{};
        auto expected{ origin + static_cast<std::uint64_t>(delays[axis]->count() / 100) };
        std::uint64_t last{};
        for (const auto edge : hardware->edges) {
            if (edge.axis != axis)
                continue;
            if (edge.high) {
                EXPECT_EQ(edge.tick, expected);
                last = edge.tick;
                expected += sequences[axis]->timing(pulse++)->period.count() / 100;
            }
            else
                EXPECT_EQ(edge.tick - last, 50U);
        }
        EXPECT_EQ(pulse, counts[axis]);
        EXPECT_EQ(last, final);
        EXPECT_FALSE(hardware->high[axis]);
    }
    const auto state{ generator->status() };
    EXPECT_TRUE(state.counts_exact);
    for (unsigned i{}; i < 3U; ++i) {
        EXPECT_EQ(state.axes[i], hal::step::State::Completed);
        EXPECT_EQ(state.pulses[i], counts[i]);
    }
}

TEST_F(RobotState, CoordinatedMovesRequireEnabledReferencedIdleAxes)
{
    configure();
    control::Robot robot{ controller, geometry };
    EXPECT_THROW(robot.moveJoints({ 0_deg, 90_deg, 10_mm }), std::runtime_error);
    robot.enable();
    EXPECT_THROW(robot.moveJoints({ 0_deg, 90_deg, 10_mm }), std::runtime_error);
    const auto independent{ controller->move(Motor1, { .position = -360_deg, .velocity = 5_rpm }) };
    EXPECT_THROW(robot.reference(), std::runtime_error);
    controller->stop();
    EXPECT_EQ(independent.completion.get(), StepperMotor::Result::Stopped);
    EXPECT_THROW(robot.moveJoints({}, { .speed = 0 }), std::invalid_argument);
    EXPECT_THROW(robot.moveJoints({}, { .speed = 1.01 }), std::invalid_argument);
    EXPECT_THROW(robot.reference(0_s), std::invalid_argument);
}

TEST_F(RobotState, ReferencesInOrderThenExecutesExactSynchronizedJointAndCartesianTargets)
{
    configure();
    control::Robot robot{ controller, geometry };
    ASSERT_TRUE(home(robot));
    ASSERT_TRUE(robot.status().referenced);
    const Kinematics::JointPosition target{ -72_deg, 141_deg, 0.5_mm }; // +9,+18,+36 motor degrees
    auto move{ robot.moveJoints(target, { .speed = 1 }) };
    ASSERT_EQ(move.completion.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(move.completion.get(), StepperMotor::Result::Completed);
    auto state{ robot.status() };
    EXPECT_NEAR(state.commanded.joints.shoulder / 1_deg, -72, 1e-8);
    EXPECT_NEAR(state.commanded.joints.elbow / 1_deg, 141, 1e-8);
    EXPECT_NEAR(state.commanded.joints.z / 1_mm, 0.5, 1e-8);
    EXPECT_EQ(state.motors.generator.pulses, (std::array<hal::step::PulseCount, 3>{ 80, 160, 320 }));
    EXPECT_FALSE(state.motors.coordinated);
    EXPECT_EQ(state.commanded.joint_velocity.shoulder, 0_rpm);
    auto cartesian{ *robot.kinematics().forward({ -70_deg, 139_deg, 1_mm }) };
    move = robot.moveAbs(cartesian.position, { .speed = 1 });
    ASSERT_EQ(move.completion.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(move.completion.get(), StepperMotor::Result::Completed);
    const auto before{ robot.status() };
    move = robot.moveRel({ 0_mm, 0_mm, 1_mm }, { .speed = 1 });
    ASSERT_EQ(move.completion.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(move.completion.get(), StepperMotor::Result::Completed);
    state = robot.status();
    EXPECT_NEAR(state.commanded.joints.z / 1_mm, before.commanded.joints.z / 1_mm + 1, 1e-8);
    EXPECT_NEAR(state.commanded.joints.shoulder / 1_deg, before.commanded.joints.shoulder / 1_deg, 1e-8);
    EXPECT_TRUE(state.referenced);
    EXPECT_EQ(robot.motions().size(), 4U);
}

TEST_F(RobotState, GroupOwnershipRejectsConflictsAndStoppingOneAxisCancelsAll)
{
    configure();
    control::Robot robot{ controller, geometry };
    ASSERT_TRUE(home(robot));
    const auto move{ robot.moveJoints({ -100_deg, 170_deg, -10_mm }, { .speed = 0.1 }) };
    ASSERT_TRUE(eventually([&] { return robot.status().motors.coordinated; }));
    EXPECT_THROW(controller->move(Motor1, { .position = -1_deg, .velocity = 1_rpm }), std::runtime_error);
    EXPECT_THROW(controller->reference(Motor3), std::runtime_error);
    EXPECT_THROW(controller->setVelocity(Motor1, 1_rpm), std::runtime_error);
    EXPECT_THROW(controller->setDefaults(Motor1, {}), std::runtime_error);
    EXPECT_THROW(robot.moveJoints({}), std::runtime_error);
    controller->stop(Motor2);
    ASSERT_EQ(move.completion.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(move.completion.get(), StepperMotor::Result::Stopped);
    const auto state{ robot.status() };
    EXPECT_FALSE(state.motors.coordinated);
    for (auto axis : state.motors.generator.axes)
        EXPECT_NE(axis, hal::step::State::Running);
    const auto independent{ controller->move(Motor3, { .position = -1_deg, .velocity = 5_rpm }) };
    ASSERT_EQ(independent.completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(independent.completion.get(), StepperMotor::Result::Completed);
}

TEST_F(RobotState, SwitchActivationStopsTheGroupAndAnActiveSwitchAllowsRetreat)
{
    configure();
    control::Robot robot{ controller, geometry };
    ASSERT_TRUE(home(robot));
    const auto input{ hal::gpio::simulatedInput({ hal::gpio::Port::E, 7U }) };
    input->setSimulatedLevel(hal::gpio::Level::High);
    EXPECT_THROW(robot.moveJoints({ -80_deg, 145_deg, 0_mm }), std::runtime_error);
    auto move{ robot.moveJoints({ -60_deg, 130_deg, 3_mm }, { .speed = 1 }) }; // All motor shafts retreat.
    ASSERT_EQ(move.completion.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(move.completion.get(), StepperMotor::Result::Completed);
    input->setSimulatedLevel(hal::gpio::Level::Low);
    move = robot.moveJoints({ -100_deg, 170_deg, -5_mm }, { .speed = 0.2 });
    ASSERT_TRUE(eventually([&] { return robot.status().motors.axes[0].velocity > 0_rpm; }));
    input->setSimulatedLevel(hal::gpio::Level::High);
    ASSERT_EQ(move.completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(move.completion.get(), StepperMotor::Result::Stopped);
    for (auto axis : robot.status().motors.generator.axes)
        EXPECT_NE(axis, hal::step::State::Running);
}

TEST_F(RobotState, TimeoutNoOpUnreachableTargetAndDisableRetainCorrectState)
{
    configure();
    control::Robot robot{ controller, geometry };
    ASSERT_TRUE(home(robot));
    const auto before{ robot.status() };
    EXPECT_THROW(robot.moveAbs({ 10000_mm, 0_mm, 0_mm }), std::invalid_argument);
    const auto no_op{ robot.moveJoints(before.commanded.joints) };
    EXPECT_EQ(no_op.completion.get(), StepperMotor::Result::Completed);
    auto move{ robot.moveJoints({ -100_deg, 170_deg, -5_mm }, { .speed = 0.1, .timeout = 0.02_s }) };
    ASSERT_EQ(move.completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(move.completion.get(), StepperMotor::Result::TimedOut);
    move = robot.moveJoints({ -100_deg, 170_deg, -5_mm }, { .speed = 0.1 });
    robot.disable();
    EXPECT_EQ(move.completion.get(), StepperMotor::Result::Stopped);
    EXPECT_FALSE(robot.status().referenced);
    EXPECT_FALSE(robot.status().motors.enabled);
    EXPECT_FALSE(robot.status().motors.coordinated);
}

TEST_F(RobotState, HomingTimeoutAndCancellationReleaseGroupOwnership)
{
    configure();
    control::Robot robot{ controller, geometry };
    robot.enable();
    auto home{ robot.reference(0.05_s) };
    ASSERT_EQ(home.completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(home.completion.get(), StepperMotor::Result::TimedOut);
    home = robot.reference();
    robot.stop();
    EXPECT_EQ(home.completion.get(), StepperMotor::Result::Stopped);
    EXPECT_FALSE(robot.status().motors.coordinated);
    EXPECT_FALSE(robot.status().referenced);
}

TEST_F(RobotState, QuantizedEndpointMustStillFitJointLimits)
{
    configure();
    auto limited{ geometry };
    limited.limits = Kinematics::JointLimits{ { -80_deg, 130_deg, 0_mm },
                                             { -60_deg, 140.06_deg, 4_mm } };
    control::Robot robot{ controller, limited };
    ASSERT_TRUE(home(robot));
    const auto before{ robot.status() };
    // Elbow resolution is 0.0375 deg. The requested 140.055 is within the
    // limit, but its nearest reachable step (140.0625) exceeds it.
    EXPECT_THROW(robot.moveJoints({ -70_deg, 140.055_deg, 2_mm }), std::invalid_argument);
    EXPECT_EQ(robot.status().motors.generator.pulses, before.motors.generator.pulses);
    EXPECT_FALSE(robot.status().motors.coordinated);
}

TEST_F(RobotState, EncoderFaultCancelsRobotMotionEvenBeforeItsFirstPulse)
{
    configure();
    control::Robot robot{ controller, geometry };
    ASSERT_TRUE(home(robot));
    const auto move{ robot.moveJoints({ -100_deg, 170_deg, -5_mm }, { .speed = 0.1 }) };
    const auto encoder{ hal::encoder::simulatedEncoder(3U) };
    ASSERT_TRUE(encoder);
    ASSERT_TRUE(encoder->stop());
    ASSERT_TRUE(encoder->setPosition(std::numeric_limits<std::int64_t>::max()));
    ASSERT_TRUE(encoder->start());
    EXPECT_FALSE(encoder->advanceSimulatedCounts(1));
    ASSERT_EQ(move.completion.wait_for(1s), std::future_status::ready);
    EXPECT_NE(move.completion.get(), StepperMotor::Result::Completed);
    EXPECT_FALSE(robot.status().referenced);
    for (auto axis : robot.status().motors.generator.axes)
        EXPECT_NE(axis, hal::step::State::Running);
}

TEST_F(RobotState, HomingStopsIfAnAlreadyReferencedAxisLosesEncoderFeedback)
{
    configure();
    control::Robot robot{ controller, geometry };
    robot.enable();
    const auto operation{ robot.reference(5_s) };
    // Complete Z and shoulder, then inject a shoulder encoder fault while
    // the coordinator is waiting for the elbow's first switch contact.
    for (const auto index : { 2U, 0U }) {
        const auto input{ hal::gpio::simulatedInput(
          { hal::gpio::Port::E, static_cast<std::uint8_t>(index == 2U ? 10U : 7U) }) };
        ASSERT_TRUE(eventually([&] { return controller->status().axes[index].velocity > 0_rpm; }));
        input->setSimulatedLevel(hal::gpio::Level::High);
        ASSERT_TRUE(eventually([&] { return controller->status().axes[index].velocity < 0_rpm; }));
        input->setSimulatedLevel(hal::gpio::Level::Low);
        ASSERT_TRUE(eventually([&] { return controller->status().axes[index].velocity > 0_rpm; }));
        input->setSimulatedLevel(hal::gpio::Level::High);
        ASSERT_TRUE(eventually([&] { return controller->status().axes[index].referenced; }));
        input->setSimulatedLevel(hal::gpio::Level::Low);
    }
    ASSERT_TRUE(eventually([&] { return controller->status().axes[1].velocity > 0_rpm; }));
    const auto encoder{ hal::encoder::simulatedEncoder(3U) };
    ASSERT_TRUE(encoder->stop());
    ASSERT_TRUE(encoder->setPosition(std::numeric_limits<std::int64_t>::max()));
    ASSERT_TRUE(encoder->start());
    EXPECT_FALSE(encoder->advanceSimulatedCounts(1));
    ASSERT_EQ(operation.completion.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(operation.completion.get(), StepperMotor::Result::Faulted);
    EXPECT_FALSE(robot.status().motors.coordinated);
    for (auto axis : robot.status().motors.generator.axes)
        EXPECT_NE(axis, hal::step::State::Running);
}
