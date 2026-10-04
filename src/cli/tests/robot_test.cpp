#include "cli/axis.hpp"
#include "cli/robot.hpp"

#include <gtest/gtest.h>

#include <sstream>

namespace
{
    using Controller = control::MotionController;
    using Kinematics = ScaraKinematics;
    using enum Controller::MotorId;
    using namespace pnm::units;
    using namespace pnm::units::literals;

    class CliRobot : public testing::Test
    {
      protected:
        std::shared_ptr<Controller> controller;
        std::shared_ptr<control::Robot> robot;
        cli::Parser parser;
        std::ostringstream output;

        void SetUp() override
        {
            controller = std::make_shared<Controller>(std::array<Controller::AxisConfig, 3>{ {
              { 135_deg, 1.8_deg, 16U },
              { 135_deg, 1.8_deg, 16U },
              { 135_deg, 1.8_deg, 16U },
            } });
            controller->configureAxis(Motor1,
                                      RotaryAxisConversion{ {
                                        .motor_revolutions_per_axis_revolution = 1,
                                        .motor_reference = 135_deg,
                                        .axis_reference = 135_deg,
                                      } });
            controller->configureAxis(Motor2,
                                      RotaryAxisConversion{ {
                                        .motor_revolutions_per_axis_revolution = 1,
                                        .motor_reference = 135_deg,
                                        .axis_reference = 225_deg,
                                      } });
            controller->configureAxis(Motor3,
                                      LinearAxisConversion{ {
                                        .travel_per_output_revolution = 40_mm,
                                        .motor_reference = 135_deg,
                                        .axis_reference = 25_mm,
                                      } });
            robot = std::make_shared<control::Robot>(
              controller, Kinematics::Config{ .first_arm_length = 200_mm, .second_arm_length = 100_mm });
            cli::motion::setup(parser, controller);
            cli::axis::setup(parser, controller);
            cli::robot::setup(parser, robot);
        }

        cli::ExecutionResult run(std::string_view command)
        {
            output.str({});
            output.clear();
            return parser.execute(command, output);
        }
    };
}

TEST_F(CliRobot, HelpAndGeometryExplainCoordinateUnitsAndReadOnlyScope)
{
    ASSERT_TRUE(run("robot"));
    EXPECT_NE(output.str().find("distances in mm"), std::string::npos);
    EXPECT_NE(output.str().find("do not move"), std::string::npos);
    ASSERT_TRUE(run("help robot ik"));
    EXPECT_NE(output.str().find("--branch"), std::string::npos);
    ASSERT_TRUE(run("robot geometry"));
    EXPECT_NE(output.str().find("first=200.0000 mm"), std::string::npos);
    EXPECT_NE(output.str().find("second=100.0000 mm"), std::string::npos);
    EXPECT_NE(output.str().find("Joint limits: not configured"), std::string::npos);
    EXPECT_FALSE(parser.contains("axis configure"));
    EXPECT_FALSE(parser.contains("robot move"));
}

TEST_F(CliRobot, ForwardAndInverseUseSharedGeometryAndPreserveLiveBranch)
{
    ASSERT_TRUE(run("robot fk 0 90 10"));
    EXPECT_NE(output.str().find("x=200.0000 mm  y=100.0000 mm  z=10.0000 mm"), std::string::npos);
    EXPECT_NE(output.str().find("yaw=90.0000 deg"), std::string::npos);
    ASSERT_TRUE(run("robot ik 200 100 10"));
    EXPECT_NE(output.str().find("elbow=90.0000 deg"), std::string::npos);
    EXPECT_NE(output.str().find("Seed is unreferenced"), std::string::npos);
    ASSERT_TRUE(run("robot ik 200 100 10 --branch negative"));
    EXPECT_NE(output.str().find("shoulder=53.1301 deg"), std::string::npos);
    // The 90-degree seed is halfway between -90 and 270; nearest-turn rounding
    // chooses 270. Branch is the bend's sign, not the unwrapped angle's sign.
    EXPECT_NE(output.str().find("elbow=270.0000 deg"), std::string::npos);
    ASSERT_TRUE(run("robot ik 200 100 10 --branch positive"));
    EXPECT_NE(output.str().find("elbow=90.0000 deg"), std::string::npos);
    EXPECT_TRUE(controller->motions().empty());
    EXPECT_FALSE(controller->status().enabled);
}

TEST_F(CliRobot, StateShowsReferenceValidityAndDoesNotInventEncoderCoordinates)
{
    ASSERT_TRUE(run("robot status"));
    EXPECT_NE(output.str().find("referenced=no"), std::string::npos);
    EXPECT_NE(output.str().find("x=200.0000 mm  y=100.0000 mm  z=10.0000 mm"), std::string::npos);
    EXPECT_NE(output.str().find("elbow encoder not fitted"), std::string::npos);
    ASSERT_TRUE(run("motor enable"));
    ASSERT_TRUE(run("robot status"));
    EXPECT_NE(output.str().find("Drivers: enabled"), std::string::npos);
    ASSERT_TRUE(run("axis disable"));
    ASSERT_TRUE(run("robot status"));
    EXPECT_NE(output.str().find("Drivers: disabled"), std::string::npos);
    EXPECT_TRUE(controller->motions().empty());
}

TEST_F(CliRobot, RejectsUnreachableSingularMalformedAndAmbiguousRequests)
{
    const auto unreachable{ run("robot ik 1000 0 0") };
    EXPECT_FALSE(unreachable);
    EXPECT_NE(unreachable.message.find("reachable workspace"), std::string::npos);
    const auto singular{ run("robot ik 300 0 0") };
    EXPECT_FALSE(singular);
    EXPECT_NE(singular.message.find("singularity"), std::string::npos);
    for (const auto command : { "robot fk nan 0 0",
                                "robot fk 0 inf 0",
                                "robot ik 200 100 nan",
                                "robot ik 200 100",
                                "robot fk 0 90 10 extra",
                                "robot ik 200 100 0 --branch other",
                                "robot ik 200 100 0 --branch positive --branch negative",
                                "robot ik 200 100 0 --speed 10" }) {
        SCOPED_TRACE(command);
        EXPECT_FALSE(run(command));
    }
    // At a singular seed the caller must choose an elbow branch explicitly.
    controller->configureAxis(Motor2,
                              RotaryAxisConversion{ {
                                .motor_revolutions_per_axis_revolution = 1,
                                .motor_reference = 135_deg,
                                .axis_reference = 135_deg,
                              } });
    EXPECT_FALSE(run("robot ik 200 100 0"));
    ASSERT_TRUE(run("robot ik 200 100 0 --branch positive"));
    EXPECT_TRUE(controller->motions().empty());
}

TEST_F(CliRobot, InverseHonorsConfiguredJointLimits)
{
    cli::Parser limited_parser;
    auto limited_robot{ std::make_shared<control::Robot>(
      controller,
      Kinematics::Config{
        .first_arm_length = 200_mm,
        .second_arm_length = 100_mm,
        .limits = Kinematics::JointLimits{ { -180_deg, -170_deg, 0_mm }, { 180_deg, -10_deg, 100_mm } } }) };
    cli::robot::setup(limited_parser, limited_robot);
    const auto rejected{ limited_parser.execute("robot ik 200 100 10", output) };
    EXPECT_FALSE(rejected);
    EXPECT_NE(rejected.message.find("joint limit exceeded"), std::string::npos);
    EXPECT_TRUE(limited_parser.execute("robot ik 200 100 10 --branch negative", output));
    EXPECT_TRUE(controller->motions().empty());
}
