#include "cli/axis.hpp"
#include "cli/motor.hpp"
#include "cli/robot.hpp"
#include "hal/drivers/impl/linux/Gpio.hpp"

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
            cli::motor::setup(parser, controller);
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
    EXPECT_TRUE(parser.contains("robot move"));
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

TEST_F(CliRobot, MotionCommandsAreAnnotatedAndRejectInvalidOrUnreferencedRequests)
{
    for (const auto command : { "robot move", "robot moveto", "robot joints", "robot home", "robot jobs" }) {
        ASSERT_TRUE(parser.contains(command));
        ASSERT_TRUE(run(std::string("help ") + command));
    }
    ASSERT_TRUE(run("help robot moveto"));
    EXPECT_NE(output.str().find("--speed"), std::string::npos);
    EXPECT_NE(output.str().find("--branch"), std::string::npos);
    ASSERT_TRUE(run("robot enable"));
    for (const auto command : { "robot move 1 2 3",
                                "robot moveto 200 100 0",
                                "robot joints 0 90 0",
                                "robot joints 0 90 0 --speed 101",
                                "robot joints 0 90 0 --speed 0",
                                "robot joints 0 90 0 --branch positive",
                                "robot home --timeout 0",
                                "robot move nan 0 0",
                                "robot jobs abc",
                                "robot jobs 123" }) {
        SCOPED_TRACE(command);
        EXPECT_FALSE(run(command));
    }
    ASSERT_TRUE(run("robot jobs"));
    EXPECT_NE(output.str().find("No robot operations"), std::string::npos);
    ASSERT_TRUE(run("robot home --timeout 0.05"));
    EXPECT_NE(output.str().find("Robot #1 submitted"), std::string::npos);
    const auto job{ robot->motions().front() };
    ASSERT_EQ(job.completion.wait_for(std::chrono::seconds{ 2 }), std::future_status::ready);
    EXPECT_EQ(run("robot jobs 1").exit_code, 1);
    EXPECT_NE(output.str().find("timed-out"), std::string::npos);
    ASSERT_TRUE(run("robot disable"));
    ASSERT_TRUE(run("robot reset"));
    ASSERT_TRUE(run("robot stop"));
    EXPECT_FALSE(controller->status().enabled);
}

TEST_F(CliRobot, HomesAndMovesThroughAnnotatedCommandsUsingTheSameAxes)
{
    using namespace std::chrono_literals;
    const auto wait = [&](auto predicate) {
        const auto deadline{std::chrono::steady_clock::now()+2s};
        do {
            if (predicate()) return true;
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now()<deadline);
        return false;
    };
    std::array<std::shared_ptr<hal::GpioInput>,3> switches;
    constexpr std::array pins{7U,8U,10U};
    for (std::size_t i{}; i<pins.size(); ++i) {
        switches[i]=hal::gpio::simulatedInput({hal::gpio::Port::E,static_cast<std::uint8_t>(pins[i])});
        ASSERT_TRUE(switches[i]);
        switches[i]->setSimulatedLevel(hal::gpio::Level::Low);
    }
    ASSERT_TRUE(run("robot enable"));
    ASSERT_TRUE(run("robot home --timeout 5"));
    for (auto i:{2U,0U,1U}) {
        ASSERT_TRUE(wait([&]{return controller->status().axes[i].velocity>0_rpm;}));
        switches[i]->setSimulatedLevel(hal::gpio::Level::High);
        ASSERT_TRUE(wait([&]{return controller->status().axes[i].velocity<0_rpm;}));
        switches[i]->setSimulatedLevel(hal::gpio::Level::Low);
        ASSERT_TRUE(wait([&]{return controller->status().axes[i].velocity>0_rpm;}));
        switches[i]->setSimulatedLevel(hal::gpio::Level::High);
        ASSERT_TRUE(wait([&]{return controller->status().axes[i].referenced;}));
        switches[i]->setSimulatedLevel(hal::gpio::Level::Low);
    }
    ASSERT_EQ(robot->motions().front().completion.wait_for(2s),std::future_status::ready);
    ASSERT_TRUE(run("robot joints 130 220 24 --speed 100"));
    auto job{robot->motions().back()};
    ASSERT_EQ(job.completion.wait_for(3s),std::future_status::ready);
    ASSERT_EQ(job.completion.get(),StepperMotor::Result::Completed);
    ASSERT_TRUE(run("robot jobs 2"));
    EXPECT_NE(output.str().find("completed"),std::string::npos);
    ASSERT_TRUE(run("axis status z"));
    EXPECT_NE(output.str().find("position=24.0000 mm"),std::string::npos);
    ASSERT_TRUE(run("robot move 0 0 1 --speed 100"));
    job=robot->motions().back();
    ASSERT_EQ(job.completion.wait_for(3s),std::future_status::ready);
    EXPECT_EQ(job.completion.get(),StepperMotor::Result::Completed);
    EXPECT_NEAR(robot->status().commanded.joints.z/1_mm,25,1e-8);
    ASSERT_TRUE(run("robot joints 100 190 10 --speed 1"));
    job=robot->motions().back();
    EXPECT_FALSE(run("axis move z 1 --speed 1"));
    EXPECT_FALSE(run("motor speed m1 1"));
    ASSERT_TRUE(run("axis stop shoulder"));
    ASSERT_EQ(job.completion.wait_for(2s),std::future_status::ready);
    EXPECT_EQ(job.completion.get(),StepperMotor::Result::Stopped);
    ASSERT_TRUE(run("robot disable"));
}
