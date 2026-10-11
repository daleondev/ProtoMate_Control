// Numeric values describe explicit test inputs, timing bounds and expected results.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
#include "control/Robot.hpp"
#include "control/SynchronizedSequence.hpp"
#include "control/tests/Connection.hpp"
#include "hal/drivers/impl/StepGenerator.hpp"
#include "hal/drivers/impl/linux/Gpio.hpp"
#include "hal/drivers/impl/linux/QuadratureEncoder.hpp"
#include "hal/drivers/impl/linux/SimulatedStepHardware.hpp"

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

    const Kinematics::Config GEOMETRY{ .first_arm_length = 200_mm, .second_arm_length = 100_mm };

    struct RobotState : public testing::Test
    {
        std::shared_ptr<Controller> m_controller;
        std::unique_ptr<control::test::Connection> m_connection;

        void SetUp() override
        {
            m_controller = std::make_shared<Controller>(std::array<Controller::AxisConfig, 3>{ {
              { .reference_position = 135_deg, .full_step_angle = 1.8_deg, .microsteps = 16U },
              { .reference_position = 135_deg, .full_step_angle = 1.8_deg, .microsteps = 16U },
              { .reference_position = 135_deg, .full_step_angle = 1.8_deg, .microsteps = 16U },
            } });
            auto alarm = hal::gpio::simulated_input({ .port = hal::gpio::Port::F, .number = 2 });
            ASSERT_TRUE(alarm);
            alarm->setSimulatedLevel(hal::gpio::Level::Low); // Healthy, powered DM542T.
            m_controller->initializeDrivers();
            for (const auto pin : { 7U, 8U, 10U }) {
                auto input{ hal::gpio::simulated_input(
                  { .port = hal::gpio::Port::E, .number = static_cast<std::uint8_t>(pin) }) };
                ASSERT_TRUE(input);
                input->setSimulatedLevel(hal::gpio::Level::Low);
            }
            m_connection = std::make_unique<control::test::Connection>(m_controller);
        }

        void configure()
        {
            // Zero motor angles map to joint coordinates (0 deg, 90 deg, 10 mm).
            // Non-unit reductions and inverted shoulder/Z exercise all conversions.
            m_controller->configureAxis(Motor1,
                                        RotaryAxisConversion{ {
                                          .motor_revolutions_per_axis_revolution = 2,
                                          .direction = AxisDirection::OppositeToMotor,
                                          .motor_reference = 135_deg,
                                          .axis_reference = -67.5_deg,
                                        } });
            m_controller->configureAxis(Motor2,
                                        RotaryAxisConversion{ {
                                          .motor_revolutions_per_axis_revolution = 3,
                                          .motor_reference = 135_deg,
                                          .axis_reference = 135_deg,
                                        } });
            m_controller->configureAxis(Motor3,
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
                const auto input{ hal::gpio::simulated_input(
                  { .port = hal::gpio::Port::E, .number = static_cast<std::uint8_t>(pins[index]) }) };
                if (!eventually([&] { return m_controller->status().axes[index].velocity > 0_rpm; }))
                    return false;
                input->setSimulatedLevel(hal::gpio::Level::High);
                if (!eventually([&] { return m_controller->status().axes[index].velocity < 0_rpm; }))
                    return false;
                input->setSimulatedLevel(hal::gpio::Level::Low);
                if (!eventually([&] { return m_controller->status().axes[index].velocity > 0_rpm; }))
                    return false;
                input->setSimulatedLevel(hal::gpio::Level::High);
                if (!eventually([&] { return m_controller->status().axes[index].referenced; }))
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
            for (;;) {
                if (predicate())
                    return true;
                std::this_thread::sleep_for(1ms);
                if (!(std::chrono::steady_clock::now() < deadline))
                    break;
            }
            return false;
        }
    };
}

TEST(Robot, RejectsMissingController)
{
    EXPECT_THROW((control::Robot{ nullptr, GEOMETRY }), std::invalid_argument);
}

TEST_F(RobotState, RequiresMechanicsForLiveStateButNotForGeometryCalculations)
{
    const control::Robot robot{ m_connection->client, GEOMETRY };
    EXPECT_THROW(robot.status(), std::runtime_error);
    ASSERT_TRUE(robot.kinematics().forward({ 0_deg, 90_deg, 10_mm }));
    configure();
    EXPECT_NO_THROW(robot.status());
}

TEST_F(RobotState, ConvertsOffsetsReductionsAndDirectionsBeforeComputingToolState)
{
    configure();
    const control::Robot robot{ m_connection->client, GEOMETRY };
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
    EXPECT_TRUE(m_controller->motions().empty());
}

TEST_F(RobotState, LiveVelocityAndPositionTrackSharedMotorMotionAndStop)
{
    configure();
    const control::Robot robot{ m_connection->client, GEOMETRY };
    m_controller->enable();
    auto move{ m_controller->move(Motor3, { .position = -360_deg, .velocity = 30_rpm }) };
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
    m_controller->stop(Motor3);
    ASSERT_EQ(move.completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(move.completion.get(), StepperMotor::Result::Stopped);
    const auto stopped{ robot.status() };
    EXPECT_GT(stopped.commanded.joints.z, 10_mm);
    EXPECT_EQ(stopped.commanded.joint_velocity.z, 0_mm_s);
    ASSERT_TRUE(stopped.commanded.pose);
    EXPECT_EQ(stopped.commanded.pose->position.z, stopped.commanded.joints.z);
    EXPECT_NEAR(stopped.commanded.joints.z / 1_mm, 10 - (stopped.motors.axes[2].position / 1_deg / 18), 1e-9);
}

TEST_F(RobotState, FeedbackRemainsDistinctFromCommandedStateAndPropagatesFaults)
{
    configure();
    const control::Robot robot{ m_connection->client, GEOMETRY };
    auto encoder{ hal::encoder::simulated_encoder(hal::timer::Peripheral::Tim3) };
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
    const control::Robot robot{ m_connection->client, GEOMETRY };
    const auto before{ robot.status() };
    m_controller->configureAxis(Motor3,
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
    auto profile{ motion::Profile::create(
      1.0, { .velocity = 0.73, .acceleration = 1.1, .deceleration = 0.6, .jerk = 4.2 }) };
    if (!profile) {
        FAIL() << "expected profile to have a value";
    }
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
    auto port{ std::make_unique<hal::util::SimulatedStepHardware>() };
    auto* hardware{ port.get() };
    auto generator{ std::make_shared<hal::util::StepGenerator>(std::move(port)) };
    hardware->interrupt = [&] { generator->service(); };
    const auto profile{ motion::Profile::create(
      1.0, { .velocity = 0.73, .acceleration = 1.1, .deceleration = 0.6, .jerk = 4.2 }) };
    if (!profile) {
        FAIL() << "expected profile to have a value";
    }
    constexpr std::array counts{ 7U, 641U, 1753U };
    std::array<std::shared_ptr<motion::SynchronizedSequence>, 3> sequences;
    std::array<std::optional<std::chrono::nanoseconds>, 3> delays;
    ASSERT_TRUE(generator->start());
    hardware->advance(hal::util::STEP_PARK - 1000U);
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
        const auto delay{ delays[axis] };
        if (!delay) {
            FAIL() << "expected delays[axis] to have a value";
        }
        auto expected{ origin + static_cast<std::uint64_t>(delay->count() / 100) };
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
    control::Robot robot{ m_connection->client, GEOMETRY };
    EXPECT_THROW(robot.moveJoints({ 0_deg, 90_deg, 10_mm }), std::runtime_error);
    robot.enable();
    EXPECT_THROW(robot.moveJoints({ 0_deg, 90_deg, 10_mm }), std::runtime_error);
    const auto independent{ m_controller->move(Motor1, { .position = -360_deg, .velocity = 5_rpm }) };
    EXPECT_THROW(robot.reference(), std::runtime_error);
    m_controller->stop();
    EXPECT_EQ(independent.completion.get(), StepperMotor::Result::Stopped);
    EXPECT_THROW(robot.moveJoints({}, { .speed = 0 }), std::invalid_argument);
    EXPECT_THROW(robot.moveJoints({}, { .speed = 1.01 }), std::invalid_argument);
    EXPECT_THROW(robot.reference(0_s), std::invalid_argument);
}

TEST_F(RobotState, ReferencesInOrderThenExecutesExactSynchronizedJointAndCartesianTargets)
{
    configure();
    control::Robot robot{ m_connection->client, GEOMETRY };
    ASSERT_TRUE(home(robot));
    ASSERT_TRUE(robot.status().referenced);
    const Kinematics::JointPosition target{ .shoulder = -72_deg,
                                            .elbow = 141_deg,
                                            .z = 0.5_mm }; // +9,+18,+36 motor degrees
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
    auto cartesian{ *robot.kinematics().forward({ .shoulder = -70_deg, .elbow = 139_deg, .z = 1_mm }) };
    move = robot.moveAbs(cartesian.position, { .speed = 1 });
    ASSERT_EQ(move.completion.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(move.completion.get(), StepperMotor::Result::Completed);
    const auto before{ robot.status() };
    move = robot.moveRel({ .x = 0_mm, .y = 0_mm, .z = 1_mm }, { .speed = 1 });
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
    control::Robot robot{ m_connection->client, GEOMETRY };
    ASSERT_TRUE(home(robot));
    const auto move{ robot.moveJoints({ .shoulder = -100_deg, .elbow = 170_deg, .z = -10_mm },
                                      { .speed = 0.1 }) };
    ASSERT_TRUE(eventually([&] { return robot.status().motors.coordinated; }));
    EXPECT_THROW(m_controller->move(Motor1, { .position = -1_deg, .velocity = 1_rpm }), std::runtime_error);
    EXPECT_THROW(m_controller->reference(Motor3), std::runtime_error);
    EXPECT_THROW(m_controller->setVelocity(Motor1, 1_rpm), std::runtime_error);
    EXPECT_THROW(m_controller->setDefaults(Motor1, {}), std::runtime_error);
    EXPECT_THROW(robot.moveJoints({}), std::runtime_error);
    m_controller->stop(Motor2);
    ASSERT_EQ(move.completion.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(move.completion.get(), StepperMotor::Result::Stopped);
    const auto state{ robot.status() };
    EXPECT_FALSE(state.motors.coordinated);
    for (auto axis : state.motors.generator.axes)
        EXPECT_NE(axis, hal::step::State::Running);
    const auto independent{ m_controller->move(Motor3, { .position = -1_deg, .velocity = 5_rpm }) };
    ASSERT_EQ(independent.completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(independent.completion.get(), StepperMotor::Result::Completed);
}

TEST_F(RobotState, SwitchActivationStopsTheGroupAndAnActiveSwitchAllowsRetreat)
{
    configure();
    control::Robot robot{ m_connection->client, GEOMETRY };
    ASSERT_TRUE(home(robot));
    const auto input{ hal::gpio::simulated_input({ .port = hal::gpio::Port::E, .number = 7U }) };
    input->setSimulatedLevel(hal::gpio::Level::High);
    EXPECT_THROW(robot.moveJoints({ -80_deg, 145_deg, 0_mm }), std::runtime_error);
    auto move{ robot.moveJoints({ .shoulder = -60_deg, .elbow = 130_deg, .z = 3_mm },
                                { .speed = 1 }) }; // All motor shafts retreat.
    ASSERT_EQ(move.completion.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(move.completion.get(), StepperMotor::Result::Completed);
    input->setSimulatedLevel(hal::gpio::Level::Low);
    move = robot.moveJoints({ .shoulder = -100_deg, .elbow = 170_deg, .z = -5_mm }, { .speed = 0.2 });
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
    control::Robot robot{ m_connection->client, GEOMETRY };
    ASSERT_TRUE(home(robot));
    const auto before{ robot.status() };
    EXPECT_THROW(robot.moveAbs({ 10000_mm, 0_mm, 0_mm }), std::invalid_argument);
    const auto no_op{ robot.moveJoints(before.commanded.joints) };
    EXPECT_EQ(no_op.completion.get(), StepperMotor::Result::Completed);
    auto move{ robot.moveJoints({ .shoulder = -100_deg, .elbow = 170_deg, .z = -5_mm },
                                { .speed = 0.1, .timeout = 0.02_s }) };
    ASSERT_EQ(move.completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(move.completion.get(), StepperMotor::Result::TimedOut);
    move = robot.moveJoints({ .shoulder = -100_deg, .elbow = 170_deg, .z = -5_mm }, { .speed = 0.1 });
    robot.disable();
    EXPECT_EQ(move.completion.get(), StepperMotor::Result::Stopped);
    EXPECT_FALSE(robot.status().referenced);
    EXPECT_FALSE(robot.status().motors.enabled);
    EXPECT_FALSE(robot.status().motors.coordinated);
}

TEST_F(RobotState, HomingTimeoutAndCancellationReleaseGroupOwnership)
{
    configure();
    control::Robot robot{ m_connection->client, GEOMETRY };
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
    auto limited{ GEOMETRY };
    limited.limits =
      Kinematics::JointLimits{ .minimum = { .shoulder = -80_deg, .elbow = 130_deg, .z = 0_mm },
                               .maximum = { .shoulder = -60_deg, .elbow = 140.06_deg, .z = 4_mm } };
    control::Robot robot{ m_connection->client, limited };
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
    control::Robot robot{ m_connection->client, GEOMETRY };
    ASSERT_TRUE(home(robot));
    const auto move{ robot.moveJoints({ .shoulder = -100_deg, .elbow = 170_deg, .z = -5_mm },
                                      { .speed = 0.1 }) };
    const auto encoder{ hal::encoder::simulated_encoder(hal::timer::Peripheral::Tim3) };
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
    control::Robot robot{ m_connection->client, GEOMETRY };
    robot.enable();
    const auto operation{ robot.reference(5_s) };
    // Complete Z and shoulder, then inject a shoulder encoder fault while
    // the coordinator is waiting for the elbow's first switch contact.
    for (const auto index : { 2U, 0U }) {
        const auto input{ hal::gpio::simulated_input(
          { .port = hal::gpio::Port::E, .number = static_cast<std::uint8_t>(index == 2U ? 10U : 7U) }) };
        ASSERT_TRUE(eventually([&] { return m_controller->status().axes[index].velocity > 0_rpm; }));
        input->setSimulatedLevel(hal::gpio::Level::High);
        ASSERT_TRUE(eventually([&] { return m_controller->status().axes[index].velocity < 0_rpm; }));
        input->setSimulatedLevel(hal::gpio::Level::Low);
        ASSERT_TRUE(eventually([&] { return m_controller->status().axes[index].velocity > 0_rpm; }));
        input->setSimulatedLevel(hal::gpio::Level::High);
        ASSERT_TRUE(eventually([&] { return m_controller->status().axes[index].referenced; }));
        input->setSimulatedLevel(hal::gpio::Level::Low);
    }
    ASSERT_TRUE(eventually([&] { return m_controller->status().axes[1].velocity > 0_rpm; }));
    const auto encoder{ hal::encoder::simulated_encoder(hal::timer::Peripheral::Tim3) };
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

TEST_F(RobotState, StaleSerializedPlansNeverPrepareHardware)
{
    configure();
    control::Robot robot{ m_connection->client, GEOMETRY };
    ASSERT_TRUE(home(robot));
    const auto snapshot = m_connection->client->planningSnapshot();
    Controller::Trajectory plan{};
    plan.configuration_revision = snapshot.status.configuration_revision;
    plan.command_revision = snapshot.status.command_revision;
    for (unsigned i = 0; i < 3; ++i)
        plan.origin[i] = snapshot.status.axes[i].position;
    const auto pulses = m_controller->status().generator.pulses;
    m_connection->client->setDefaults(
      Motor1, { .acceleration = 720_deg_s2, .deceleration = 720_deg_s2, .jerk = 0_deg_s3 });
    try {
        m_connection->client->coordinate(plan);
        FAIL() << "stale configuration was admitted";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("stale"), std::string::npos);
    }
    plan.configuration_revision = m_connection->client->planningSnapshot().status.configuration_revision;
    m_connection->client->stop();
    try {
        m_connection->client->coordinate(plan);
        FAIL() << "stale commanded state was admitted";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("stale"), std::string::npos);
    }
    EXPECT_EQ(m_controller->status().generator.pulses, pulses);
    EXPECT_FALSE(m_controller->status().coordinated);
}

TEST_F(RobotState, LinkLossStopsLocallyAndRequiresExplicitRecovery)
{
    auto client = m_connection->client;
    client->enable();
    auto move = client->move(Motor1, { .position = -720_deg, .velocity = 10_rpm });
    ASSERT_TRUE(eventually([&] { return m_controller->status().axes[0].velocity < 0_rpm; }));
    m_connection->transport[0]->close();
    ASSERT_TRUE(eventually([&] { return m_controller->status().axes[0].velocity == 0_rpm; }));
    ASSERT_EQ(move.completion.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(move.completion.get(),
              control::MotorResult::Faulted); // Transport loss is not a stop acknowledgement.
    const auto frozen = m_controller->status().generator.pulses;
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(m_controller->status().generator.pulses, frozen);
    EXPECT_TRUE(m_controller->status().enabled); // Retain holding torque.
    // Failed bridges never reconnect or replay commands. Rebuild the connection
    // explicitly, retaining the controller's latched motion inhibit.
    m_connection.reset();
    m_connection = std::make_unique<control::test::Connection>(m_controller);
    client = m_connection->client;
    EXPECT_THROW(client->move(Motor1, { .position = -1_deg, .velocity = 10_rpm }), std::runtime_error);
    client->disable();
    (*client).reset();
    client->enable();
    const auto next = client->move(Motor1, { .position = -1_deg, .velocity = 10_rpm });
    ASSERT_EQ(next.completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(next.completion.get(), control::MotorResult::Completed);
}

TEST_F(RobotState, CatalogTelemetryCrossesTheBridge)
{
    std::optional<control::messages::State> latest_state;
    std::optional<control::messages::FaultState> latest_faults;
    auto state = m_connection->ui_bus.topic<^^control::messages::Topics::state>().subscribe(
      [&](const auto& value) { latest_state = value; }, pnm::msg::SubscriptionOptions::latestOnly());
    auto faults = m_connection->ui_bus.topic<^^control::messages::Topics::fault>().subscribe(
      [&](const auto& value) { latest_faults = value; }, pnm::msg::SubscriptionOptions::latestOnly());
    m_connection->client->enable();
    ASSERT_TRUE(eventually([&] {
        state.poll();
        faults.poll();
        return latest_state && latest_state->enabled && latest_faults;
    }));
    EXPECT_FALSE(latest_faults->link_inhibited);
}

TEST_F(RobotState, CancellingBufferedJobPreservesIndependentActiveMotion)
{
    auto client = m_connection->client;
    client->enable();
    const auto active = client->move(Motor1, { .position = -720_deg, .velocity = 10_rpm });
    ASSERT_TRUE(eventually([&] { return m_controller->status().axes[0].velocity < 0_rpm; }));
    const auto queued = client->move(
      Motor1, { .position = -90_deg, .velocity = 10_rpm, .buffer = control::BufferMode::Buffered });
    client->cancel(queued.id);
    ASSERT_EQ(queued.completion.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(queued.completion.get(), control::MotorResult::Stopped);
    EXPECT_EQ(active.completion.wait_for(0ms), std::future_status::timeout);
    EXPECT_LT(m_controller->status().axes[0].velocity, 0_rpm);
    client->stop();
    ASSERT_EQ(active.completion.wait_for(1s), std::future_status::ready);
}

TEST(MotionProfileTransport, RoundTripPreservesNormalizedEndpointAndTiming)
{
    for (double speed : { 0.1, 1.0, 12.0 }) {
        const auto profile = motion::Profile::create(
          1, { .velocity = speed, .acceleration = 7.3, .deceleration = 4.2, .jerk = 19.0 });
        if (!profile) {
            FAIL() << "expected profile to have a value";
        }
        auto description = profile->description();
        const auto rebuilt = motion::Profile::fromDescription(description);
        if (!rebuilt) {
            FAIL() << "expected rebuilt to have a value";
        }
        EXPECT_EQ(rebuilt->distance(), 1.0);
        EXPECT_EQ(rebuilt->endVelocity(), 0.0);
        for (unsigned i = 0; i <= 100; ++i)
            EXPECT_NEAR(profile->timeAt(i / 100.0), rebuilt->timeAt(i / 100.0), 1e-12);
        description.phases[0].velocity = 1.0;
        EXPECT_FALSE(motion::Profile::fromDescription(description));
    }
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
