// Numeric values describe explicit test inputs, timing bounds and expected results.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
#include "cli/axis.hpp"
#include "cli/motor.hpp"
#include "cli/robot.hpp"
#include "control/tests/Connection.hpp"
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

    struct CliRobot : public testing::Test
    {
        std::shared_ptr<Controller> m_controller;
        std::unique_ptr<control::test::Connection> m_connection;
        std::shared_ptr<control::Robot> m_robot;
        cli::Parser m_parser;
        std::ostringstream m_output;

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
            m_controller->configureAxis(Motor1,
                                        RotaryAxisConversion{ {
                                          .motor_revolutions_per_axis_revolution = 1,
                                          .motor_reference = 135_deg,
                                          .axis_reference = 135_deg,
                                        } });
            m_controller->configureAxis(Motor2,
                                        RotaryAxisConversion{ {
                                          .motor_revolutions_per_axis_revolution = 1,
                                          .motor_reference = 135_deg,
                                          .axis_reference = 225_deg,
                                        } });
            m_controller->configureAxis(Motor3,
                                        LinearAxisConversion{ {
                                          .travel_per_output_revolution = 40_mm,
                                          .motor_reference = 135_deg,
                                          .axis_reference = 25_mm,
                                        } });
            m_connection = std::make_unique<control::test::Connection>(m_controller);
            m_robot = std::make_shared<control::Robot>(
              m_connection->client,
              Kinematics::Config{ .first_arm_length = 200_mm, .second_arm_length = 100_mm, .limits = {} });
            cli::motor::setup(m_parser, m_connection->client);
            cli::axis::setup(m_parser, m_connection->client);
            cli::robot::setup(m_parser, m_robot);
        }

        cli::ExecutionResult run(std::string_view command)
        {
            m_output.str({});
            m_output.clear();
            return m_parser.execute(command, m_output);
        }
    };
}

TEST_F(CliRobot, HelpAndGeometryExplainCoordinateUnitsAndReadOnlyScope)
{
    ASSERT_TRUE(run("robot"));
    EXPECT_NE(m_output.str().find("distances in mm"), std::string::npos);
    EXPECT_NE(m_output.str().find("do not move"), std::string::npos);
    ASSERT_TRUE(run("help robot ik"));
    EXPECT_NE(m_output.str().find("--branch"), std::string::npos);
    ASSERT_TRUE(run("robot geometry"));
    EXPECT_NE(m_output.str().find("first=200.0000 mm"), std::string::npos);
    EXPECT_NE(m_output.str().find("second=100.0000 mm"), std::string::npos);
    EXPECT_NE(m_output.str().find("Joint limits: not configured"), std::string::npos);
    EXPECT_FALSE(m_parser.contains("axis configure"));
    EXPECT_TRUE(m_parser.contains("robot move"));
}

TEST_F(CliRobot, ForwardAndInverseUseSharedGeometryAndPreserveLiveBranch)
{
    ASSERT_TRUE(run("robot fk 0 90 10"));
    EXPECT_NE(m_output.str().find("x=200.0000 mm  y=100.0000 mm  z=10.0000 mm"), std::string::npos);
    EXPECT_NE(m_output.str().find("yaw=90.0000 deg"), std::string::npos);
    ASSERT_TRUE(run("robot ik 200 100 10"));
    EXPECT_NE(m_output.str().find("elbow=90.0000 deg"), std::string::npos);
    EXPECT_NE(m_output.str().find("Seed is unreferenced"), std::string::npos);
    ASSERT_TRUE(run("robot ik 200 100 10 --branch negative"));
    EXPECT_NE(m_output.str().find("shoulder=53.1301 deg"), std::string::npos);
    // The 90-degree seed is halfway between -90 and 270; nearest-turn rounding
    // chooses 270. Branch is the bend's sign, not the unwrapped angle's sign.
    EXPECT_NE(m_output.str().find("elbow=270.0000 deg"), std::string::npos);
    ASSERT_TRUE(run("robot ik 200 100 10 --branch positive"));
    EXPECT_NE(m_output.str().find("elbow=90.0000 deg"), std::string::npos);
    EXPECT_TRUE(m_controller->motions().empty());
    EXPECT_FALSE(m_controller->status().enabled);
}

TEST_F(CliRobot, StateShowsReferenceValidityAndDoesNotInventEncoderCoordinates)
{
    ASSERT_TRUE(run("robot status"));
    EXPECT_NE(m_output.str().find("referenced=no"), std::string::npos);
    EXPECT_NE(m_output.str().find("x=200.0000 mm  y=100.0000 mm  z=10.0000 mm"), std::string::npos);
    EXPECT_NE(m_output.str().find("elbow driver INDEX waiting for INDEX"), std::string::npos);
    ASSERT_TRUE(run("motor enable"));
    ASSERT_TRUE(run("robot status"));
    EXPECT_NE(m_output.str().find("Drivers: enabled"), std::string::npos);
    ASSERT_TRUE(run("axis disable"));
    ASSERT_TRUE(run("robot status"));
    EXPECT_NE(m_output.str().find("Drivers: disabled"), std::string::npos);
    EXPECT_TRUE(m_controller->motions().empty());
}

TEST_F(CliRobot, RejectsUnreachableSingularMalformedAndAmbiguousRequests)
{
    const auto unreachable{ run("robot ik 1000 0 0") };
    EXPECT_FALSE(unreachable);
    EXPECT_NE(unreachable.message.find("reachable workspace"), std::string::npos);
    const auto singular{ run("robot ik 300 0 0") };
    EXPECT_FALSE(singular);
    EXPECT_NE(singular.message.find("singularity"), std::string::npos);
    for (const auto* const command : { "robot fk nan 0 0",
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
    m_controller->configureAxis(Motor2,
                                RotaryAxisConversion{ {
                                  .motor_revolutions_per_axis_revolution = 1,
                                  .motor_reference = 135_deg,
                                  .axis_reference = 135_deg,
                                } });
    EXPECT_FALSE(run("robot ik 200 100 0"));
    ASSERT_TRUE(run("robot ik 200 100 0 --branch positive"));
    EXPECT_TRUE(m_controller->motions().empty());
}

TEST_F(CliRobot, InverseHonorsConfiguredJointLimits)
{
    cli::Parser limited_parser;
    auto limited_robot{ std::make_shared<control::Robot>(
      m_connection->client,
      Kinematics::Config{ .first_arm_length = 200_mm,
                          .second_arm_length = 100_mm,
                          .limits = Kinematics::JointLimits{
                            .minimum = { .shoulder = -180_deg, .elbow = -170_deg, .z = 0_mm },
                            .maximum = { .shoulder = 180_deg, .elbow = -10_deg, .z = 100_mm } } }) };
    cli::robot::setup(limited_parser, limited_robot);
    const auto rejected{ limited_parser.execute("robot ik 200 100 10", m_output) };
    EXPECT_FALSE(rejected);
    EXPECT_NE(rejected.message.find("joint limit exceeded"), std::string::npos);
    EXPECT_TRUE(limited_parser.execute("robot ik 200 100 10 --branch negative", m_output));
    EXPECT_TRUE(m_controller->motions().empty());
}

TEST_F(CliRobot, MotionCommandsAreAnnotatedAndRejectInvalidOrUnreferencedRequests)
{
    for (const auto* const command :
         { "robot move", "robot moveto", "robot joints", "robot home", "robot jobs" }) {
        ASSERT_TRUE(m_parser.contains(command));
        ASSERT_TRUE(run(std::string("help ") + command));
    }
    ASSERT_TRUE(run("help robot moveto"));
    EXPECT_NE(m_output.str().find("--speed"), std::string::npos);
    EXPECT_NE(m_output.str().find("--branch"), std::string::npos);
    ASSERT_TRUE(run("robot enable"));
    for (const auto* const command : { "robot move 1 2 3",
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
    EXPECT_NE(m_output.str().find("No robot operations"), std::string::npos);
    ASSERT_TRUE(run("robot home --timeout 0.05"));
    EXPECT_NE(m_output.str().find("Robot #1 submitted"), std::string::npos);
    const auto job{ m_robot->motions().front() };
    ASSERT_EQ(job.completion.wait_for(std::chrono::seconds{ 2 }), std::future_status::ready);
    EXPECT_EQ(run("robot jobs 1").exit_code, 1);
    EXPECT_NE(m_output.str().find("timed-out"), std::string::npos);
    ASSERT_TRUE(run("robot disable"));
    ASSERT_TRUE(run("robot reset"));
    ASSERT_TRUE(run("robot stop"));
    EXPECT_FALSE(m_controller->status().enabled);
}

TEST_F(CliRobot, AxisAndRobotHomingCanBeStoppedAndRestartedRepeatedly)
{
    using namespace std::chrono_literals;
    constexpr std::array<std::uint8_t, 3> pins{ 7, 8, 10 };
    for (const auto pin : pins) {
        const auto input{ hal::gpio::simulated_input({ .port = hal::gpio::Port::E, .number = pin }) };
        ASSERT_TRUE(input);
        input->setSimulatedLevel(hal::gpio::Level::Low);
    }
    ASSERT_TRUE(run("robot enable"));
    for (unsigned repeat{}; repeat < 3U; ++repeat) {
        for (const auto* const command :
             { "axis home shoulder", "axis home elbow", "axis home z", "robot home" }) {
            SCOPED_TRACE(command);
            SCOPED_TRACE(repeat);
            ASSERT_TRUE(run(command));
            const bool group{ std::string_view{ command } == "robot home" };
            const auto completion{ group ? m_robot->motions().back().completion
                                         : m_controller->motions().back().completion };
            const auto deadline{ std::chrono::steady_clock::now() + 1s };
            bool started{};
            for (;;) {
                const auto state{ m_controller->status() };
                for (const auto& axis : state.axes)
                    started |= axis.velocity != 0_rpm;
                if (started)
                    break;
                std::this_thread::sleep_for(1ms);
                if (!(std::chrono::steady_clock::now() < deadline))
                    break;
            }
            ASSERT_TRUE(started);
            // All entry points must stop the shared operation and permit an
            // immediate new home without stale workers, notifications or jobs.
            ASSERT_TRUE(run(group ? "axis stop shoulder" : "robot stop"));
            ASSERT_EQ(completion.wait_for(0s), std::future_status::ready);
            EXPECT_EQ(completion.get(), StepperMotor::Result::Stopped);
            const auto state{ m_controller->status() };
            EXPECT_FALSE(state.coordinated);
            EXPECT_TRUE(state.generator.counts_exact);
            for (const auto& axis : state.axes) {
                EXPECT_EQ(axis.velocity, 0_rpm);
                EXPECT_FALSE(axis.referenced);
            }
        }
    }
    // A stopped home must also release the ordinary motion worker path.
    ASSERT_TRUE(run("axis move elbow -1 --speed 30"));
    const auto completion{ m_controller->motions().back().completion };
    ASSERT_EQ(completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(completion.get(), StepperMotor::Result::Completed);
    ASSERT_TRUE(run("robot disable"));
}

TEST_F(CliRobot, HomesAndMovesThroughAnnotatedCommandsUsingTheSameAxes)
{
    using namespace std::chrono_literals;
    const auto wait = [&](auto predicate) {
        const auto deadline{ std::chrono::steady_clock::now() + 2s };
        for (;;) {
            if (predicate())
                return true;
            std::this_thread::sleep_for(1ms);
            if (!(std::chrono::steady_clock::now() < deadline))
                break;
        }
        return false;
    };
    std::array<std::shared_ptr<hal::GpioInput>, 3> switches;
    constexpr std::array pins{ 7U, 8U, 10U };
    for (std::size_t i{}; i < pins.size(); ++i) {
        switches[i] = hal::gpio::simulated_input(
          { .port = hal::gpio::Port::E, .number = static_cast<std::uint8_t>(pins[i]) });
        ASSERT_TRUE(switches[i]);
        switches[i]->setSimulatedLevel(hal::gpio::Level::Low);
    }
    ASSERT_TRUE(run("robot enable"));
    ASSERT_TRUE(run("robot home --timeout 5"));
    for (auto i : { 2U, 0U, 1U }) {
        ASSERT_TRUE(wait([&] { return m_controller->status().axes[i].velocity > 0_rpm; }));
        switches[i]->setSimulatedLevel(hal::gpio::Level::High);
        ASSERT_TRUE(wait([&] { return m_controller->status().axes[i].velocity < 0_rpm; }));
        switches[i]->setSimulatedLevel(hal::gpio::Level::Low);
        ASSERT_TRUE(wait([&] { return m_controller->status().axes[i].velocity > 0_rpm; }));
        switches[i]->setSimulatedLevel(hal::gpio::Level::High);
        ASSERT_TRUE(wait([&] { return m_controller->status().axes[i].referenced; }));
        switches[i]->setSimulatedLevel(hal::gpio::Level::Low);
    }
    ASSERT_EQ(m_robot->motions().front().completion.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(run("robot joints 130 220 24 --speed 100"));
    auto job{ m_robot->motions().back() };
    ASSERT_EQ(job.completion.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(job.completion.get(), StepperMotor::Result::Completed);
    ASSERT_TRUE(run("robot jobs 2"));
    EXPECT_NE(m_output.str().find("completed"), std::string::npos);
    ASSERT_TRUE(run("axis status z"));
    EXPECT_NE(m_output.str().find("position=24.0000 mm"), std::string::npos);
    ASSERT_TRUE(run("robot move 0 0 1 --speed 100"));
    job = m_robot->motions().back();
    ASSERT_EQ(job.completion.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(job.completion.get(), StepperMotor::Result::Completed);
    EXPECT_NEAR(m_robot->status().commanded.joints.z / 1_mm, 25, 1e-8);
    ASSERT_TRUE(run("robot joints 100 190 10 --speed 1"));
    job = m_robot->motions().back();
    EXPECT_FALSE(run("axis move z 1 --speed 1"));
    EXPECT_FALSE(run("motor speed m1 1"));
    ASSERT_TRUE(run("axis stop shoulder"));
    ASSERT_EQ(job.completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(job.completion.get(), StepperMotor::Result::Stopped);
    ASSERT_TRUE(run("robot disable"));
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
