#include "control/Robot.hpp"
#include "hal/drivers/impl/linux/Gpio.hpp"
#include "hal/drivers/impl/linux/QuadratureEncoder.hpp"

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
    EXPECT_EQ(status.actual.error().reason, std::errc::no_such_device);
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
    EXPECT_EQ(state.actual.error().reason, std::errc::no_such_device);
    ASSERT_TRUE(encoder->stop());
    ASSERT_TRUE(encoder->setPosition(std::numeric_limits<std::int64_t>::max()));
    ASSERT_TRUE(encoder->start());
    EXPECT_FALSE(encoder->advanceSimulatedCounts(1));
    ASSERT_TRUE(eventually([&] {
        const auto status{ robot.status() };
        return !status.actual && status.actual.error().motor == Motor1;
    }));
    const auto faulted{ robot.status() };
    EXPECT_NE(faulted.actual.error().reason, std::errc::no_such_device);
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
