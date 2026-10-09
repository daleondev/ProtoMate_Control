#include "cli/Parser.hpp"
#include "cli/axis.hpp"
#include "cli/motor.hpp"
#include "control/MotionController.hpp"
#include "hal/drivers/impl/linux/Gpio.hpp"
#include "hal/drivers/impl/linux/QuadratureEncoder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <sstream>

namespace
{
    using Controller = control::MotionController;
    using enum Controller::MotorId;
    using namespace pnm::units;
    using namespace pnm::units::literals;
    using namespace std::chrono_literals;
    using enum hal::gpio::Level;

    class CliAxis : public testing::Test
    {
      protected:
        std::shared_ptr<Controller> controller;
        cli::Parser parser;
        std::ostringstream output;
        std::array<std::shared_ptr<hal::GpioInput>, 3> switches;

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
            constexpr std::array<std::uint8_t, 3> pins{ 7, 8, 10 };
            for (std::size_t i{}; i < pins.size(); ++i) {
                switches[i] = hal::gpio::simulatedInput({ hal::gpio::Port::E, pins[i] });
                ASSERT_TRUE(switches[i]);
                switches[i]->setSimulatedLevel(Low);
            }
            cli::motor::setup(parser, controller);
            cli::axis::setup(parser, controller);
        }

        cli::ExecutionResult run(std::string_view command)
        {
            output.str({});
            output.clear();
            return parser.execute(command, output);
        }

        void configure()
        {
            // Deliberately nontrivial test mechanics exercise reductions and inversion.
            controller->configureAxis(Motor1,
                                      RotaryAxisConversion{ { .motor_revolutions_per_axis_revolution = 5,
                                                              .direction = AxisDirection::OppositeToMotor,
                                                              .motor_reference = 135_deg,
                                                              .axis_reference = 10_deg } });
            controller->configureAxis(Motor2,
                                      RotaryAxisConversion{ { .motor_revolutions_per_axis_revolution = 1.5,
                                                              .direction = AxisDirection::SameAsMotor,
                                                              .motor_reference = 135_deg,
                                                              .axis_reference = -20_deg } });
            controller->configureAxis(Motor3,
                                      LinearAxisConversion{ { .travel_per_output_revolution = 40_mm,
                                                              .motor_revolutions_per_output_revolution = 2,
                                                              .direction = AxisDirection::OppositeToMotor,
                                                              .motor_reference = 135_deg,
                                                              .axis_reference = 200_mm } });
        }

        template<typename Predicate>
        bool eventually(Predicate predicate)
        {
            const auto deadline{ std::chrono::steady_clock::now() + 2s };
            do {
                if (predicate())
                    return true;
                std::this_thread::sleep_for(1ms);
            } while (std::chrono::steady_clock::now() < deadline);
            return false;
        }

        bool finished(std::size_t count)
        {
            return eventually([&] {
                const auto motions{ controller->motions() };
                return motions.size() == count && std::ranges::all_of(motions, [](const auto& motion) {
                    return motion.result.has_value();
                });
            });
        }
    };
}

TEST_F(CliAxis, RequiresExplicitMechanicsAndShowsUnitsInHelp)
{
    ASSERT_TRUE(run("axis"));
    EXPECT_NE(output.str().find("deg/s"), std::string::npos);
    EXPECT_NE(output.str().find("mm/s"), std::string::npos);
    ASSERT_TRUE(run("help axis move"));
    EXPECT_NE(output.str().find("mm/s^3"), std::string::npos);
    ASSERT_TRUE(run("axis status"));
    EXPECT_NE(output.str().find("mechanics not configured"), std::string::npos);
    EXPECT_FALSE(parser.contains("axis configure"));
    EXPECT_FALSE(run("axis configure a1"));
    EXPECT_FALSE(run("axis move shoulder 1 --speed 2"));
    EXPECT_FALSE(run("axis moveto z 100 --speed 2"));
    EXPECT_FALSE(run("axis home elbow"));
    EXPECT_FALSE(run("axis defaults z --accel 100"));
    EXPECT_TRUE(controller->motions().empty());
    ASSERT_TRUE(run("axis stop all"));
    ASSERT_TRUE(run("axis disable"));
    ASSERT_TRUE(run("axis reset"));
}

TEST_F(CliAxis, ConfigurationIsValidatedAndCannotChangeWhileEnabled)
{
    configure();
    const auto conversion{ controller->axisConversion(Motor1) };
    const auto& config{ std::get<RotaryAxisConversion>(conversion).configuration() };
    EXPECT_EQ(config.motor_reference, 135_deg);
    EXPECT_EQ(config.axis_reference, 10_deg);
    EXPECT_EQ(config.motor_revolutions_per_axis_revolution, 5);
    EXPECT_THROW(controller->configureAxis(Motor1,
                                           RotaryAxisConversion{ { .motor_revolutions_per_axis_revolution = 1,
                                                                   .motor_reference = 0_deg } }),
                 std::invalid_argument);
    EXPECT_THROW(controller->configureAxis(Motor1,
                                           LinearAxisConversion{ { .travel_per_output_revolution = 40_mm,
                                                                   .motor_reference = 135_deg } }),
                 std::invalid_argument);
    EXPECT_THROW(controller->configureAxis(Motor3,
                                           RotaryAxisConversion{ { .motor_revolutions_per_axis_revolution = 1,
                                                                   .motor_reference = 135_deg } }),
                 std::invalid_argument);
    ASSERT_TRUE(run("axis enable"));
    EXPECT_THROW(controller->configureAxis(Motor1,
                                           RotaryAxisConversion{ { .motor_revolutions_per_axis_revolution = 2,
                                                                   .motor_reference = 135_deg } }),
                 std::runtime_error);
    EXPECT_EQ(std::get<RotaryAxisConversion>(controller->axisConversion(Motor1))
                .configuration()
                .motor_revolutions_per_axis_revolution,
              5);
    ASSERT_TRUE(run("axis disable"));
    EXPECT_NO_THROW(controller->configureAxis(
      Motor1,
      RotaryAxisConversion{ { .motor_revolutions_per_axis_revolution = 2, .motor_reference = 135_deg } }));
}

TEST_F(CliAxis, RelativeMovesUseDisplacementsAndBothCommandGroupsShareJobs)
{
    configure();
    ASSERT_TRUE(run("motor enable"));
    ASSERT_TRUE(run("axis move a1 1.8 --speed 6 --accel 720 --decel 360 --jerk 7200"));
    ASSERT_TRUE(run("axis move z 1 --speed 5 --accel 200 --decel 100 --jerk 2000"));
    ASSERT_TRUE(run("axis move elbow -6 --speed 20"));
    ASSERT_TRUE(finished(3));
    for (const auto& motion : controller->motions())
        EXPECT_EQ(motion.result, StepperMotor::Result::Completed);
    const auto status{ controller->status() };
    EXPECT_NEAR(status.axes[0].position.get<AngleUnits::deg>(), -9, 1e-9);
    EXPECT_NEAR(status.axes[1].position.get<AngleUnits::deg>(), -9, 1e-9);
    EXPECT_NEAR(status.axes[2].position.get<AngleUnits::deg>(), -18, 1e-9);
    ASSERT_TRUE(run("axis jobs 1"));
    EXPECT_EQ(output.str(), "#1 shoulder completed\n");
    ASSERT_TRUE(run("motor jobs 1"));
    EXPECT_EQ(output.str(), "#1 m1 completed\n");
    ASSERT_TRUE(run("axis status 1"));
    EXPECT_NE(output.str().find("position=38.8000 deg"), std::string::npos);
    ASSERT_TRUE(run("axis status z"));
    EXPECT_NE(output.str().find("position=208.5000 mm"), std::string::npos);
    EXPECT_NE(output.str().find("driver INDEX (pseudo): waiting for INDEX"), std::string::npos);
    EXPECT_FALSE(run("axis moveto z 200 --speed 1"));
}

TEST_F(CliAxis, DynamicsDefaultsConvertBothWaysAndRejectUnitMismatches)
{
    configure();
    ASSERT_TRUE(run("axis defaults shoulder --accel 100 --decel 200 --jerk 1000"));
    const auto rotary{ controller->defaults(Motor1) };
    EXPECT_NEAR(rotary.acceleration.get<AngularAccelerationUnits::deg_s2>(), 500, 1e-9);
    EXPECT_NEAR(rotary.deceleration.get<AngularAccelerationUnits::deg_s2>(), 1000, 1e-9);
    EXPECT_NEAR(rotary.jerk.get<AngularJerkUnits::deg_s3>(), 5000, 1e-9);
    ASSERT_TRUE(run("axis defaults z --accel 10 --decel 20 --jerk 100"));
    const auto linear{ controller->defaults(Motor3) };
    EXPECT_NEAR(linear.acceleration.get<AngularAccelerationUnits::deg_s2>(), 180, 1e-9);
    EXPECT_NEAR(linear.deceleration.get<AngularAccelerationUnits::deg_s2>(), 360, 1e-9);
    EXPECT_NEAR(linear.jerk.get<AngularJerkUnits::deg_s3>(), 1800, 1e-9);
    ASSERT_TRUE(run("motor defaults m3 --accel 360 --decel 720 --jerk 0"));
    const auto defaults{ std::get<Controller::LinearDefaults>(controller->axisDefaults(Motor3)) };
    EXPECT_NEAR(defaults.acceleration.get<AccelerationUnits::mm_s2>(), 20, 1e-9);
    EXPECT_NEAR(defaults.deceleration.get<AccelerationUnits::mm_s2>(), 40, 1e-9);
    EXPECT_EQ(defaults.jerk, 0_mm_s3);
    EXPECT_FALSE(run("axis defaults z --accel 20 --decel -1"));
    EXPECT_EQ(controller->defaults(Motor3).deceleration, 720_deg_s2);
    EXPECT_THROW(controller->moveAxis(Motor1, Controller::LinearMove{ .position = 1_mm, .velocity = 1_mm_s }),
                 std::invalid_argument);
    EXPECT_THROW(controller->setAxisDefaults(Motor3, StepperMotor::MotionDefaults{}), std::invalid_argument);
    EXPECT_THROW(controller->setAxisVelocity(Motor3, 1_rpm), std::invalid_argument);
    EXPECT_THROW(controller->referenceAxis(Motor1, 1_mm_s, 0.5_mm_s), std::invalid_argument);
    EXPECT_TRUE(controller->motions().empty());
}

TEST_F(CliAxis, HomingUsesConvertedSpeedsAndAbsoluteReferenceOffsets)
{
    configure();
    ASSERT_TRUE(run("axis enable"));
    ASSERT_TRUE(run("axis home z --seek 2 --latch .2 --timeout 2"));
    ASSERT_TRUE(eventually([&] { return controller->status().axes[2].velocity > 0_rpm; }));
    // 2 mm/s * 18 motor deg/mm = 36 deg/s = 6 motor rpm; seek remains Forward.
    EXPECT_NEAR(controller->status().axes[2].velocity.get<AngularVelocityUnits::rpm>(), 6, 0.01);
    switches[2]->setSimulatedLevel(High);
    ASSERT_TRUE(eventually([&] { return controller->status().axes[2].velocity < 0_rpm; }));
    switches[2]->setSimulatedLevel(Low);
    ASSERT_TRUE(eventually([&] { return controller->status().axes[2].velocity > 0_rpm; }));
    EXPECT_NEAR(controller->status().axes[2].velocity.get<AngularVelocityUnits::rpm>(), 0.6, 0.01);
    switches[2]->setSimulatedLevel(High);
    ASSERT_TRUE(finished(1));
    EXPECT_EQ(controller->motions()[0].result, StepperMotor::Result::Completed);
    ASSERT_TRUE(run("axis status z"));
    EXPECT_NE(output.str().find("position=200.0000 mm"), std::string::npos);
    ASSERT_TRUE(run("axis moveto z 201 --speed 2"));
    ASSERT_TRUE(finished(2));
    EXPECT_EQ(controller->motions()[1].result, StepperMotor::Result::Completed);
    EXPECT_NEAR(controller->status().axes[2].position.get<AngleUnits::deg>(), 117, 1e-9);
    ASSERT_TRUE(run("axis disable"));
    EXPECT_FALSE(controller->status().axes[2].referenced);
    ASSERT_TRUE(run("axis enable"));
    EXPECT_FALSE(run("axis moveto z 202 --speed 2"));
}

TEST_F(CliAxis, BufferedMovesLiveSpeedAndIndependentStopUseTheSameMotor)
{
    configure();
    ASSERT_TRUE(run("axis enable"));
    ASSERT_TRUE(run("axis move shoulder 144 --speed 12"));
    ASSERT_TRUE(run("axis move z 100 --speed 2"));
    ASSERT_TRUE(eventually([&] {
        const auto status{ controller->status() };
        return status.axes[0].velocity < 0_rpm && status.axes[2].velocity < 0_rpm;
    }));
    ASSERT_TRUE(run("axis move shoulder 18 --speed 24 --buffer buffered"));
    ASSERT_TRUE(run("axis speed shoulder 24"));
    EXPECT_NE(output.str().find("Speed update scheduled"), std::string::npos);
    ASSERT_TRUE(eventually([&] {
        return std::abs(controller->status().axes[0].velocity.get<AngularVelocityUnits::rpm>() + 20) < .01;
    }));
    ASSERT_TRUE(run("axis stop shoulder"));
    EXPECT_EQ(controller->status().axes[0].velocity, 0_rpm);
    EXPECT_LT(controller->status().axes[2].velocity, 0_rpm);
    EXPECT_EQ(controller->motions()[0].result, StepperMotor::Result::Stopped);
    EXPECT_EQ(controller->motions()[2].result, StepperMotor::Result::Stopped);
    ASSERT_TRUE(run("motor stop m3"));
    EXPECT_EQ(controller->status().axes[2].velocity, 0_rpm);
}

TEST_F(CliAxis, EncoderFeedbackUsesSignedAxisConversionAndPreservesErrors)
{
    configure();
    const auto encoder{ hal::encoder::simulatedEncoder(hal::timer::Peripheral::Tim3) };
    ASSERT_TRUE(encoder);
    ASSERT_TRUE(encoder->advanceSimulatedCounts(400)); // 90 motor degrees.
    ASSERT_TRUE(eventually([&] {
        const auto value{ controller->status().axes[0].actual_position };
        return value && std::abs(value->get<AngleUnits::deg>() - 90) < 1e-9;
    }));
    ASSERT_TRUE(run("axis status shoulder"));
    EXPECT_NE(output.str().find("encoder: position=19.0000 deg"), std::string::npos);
    ASSERT_TRUE(run("axis status elbow"));
    EXPECT_NE(output.str().find("driver INDEX (pseudo): waiting for INDEX"), std::string::npos);
    ASSERT_TRUE(encoder->stop());
    ASSERT_TRUE(encoder->setPosition(std::numeric_limits<hal::IQuadratureEncoder::Count>::max()));
    ASSERT_TRUE(encoder->start());
    EXPECT_FALSE(encoder->advanceSimulatedCounts(1));
    ASSERT_TRUE(eventually([&] { return !controller->status().axes[0].actual_position; }));
    ASSERT_TRUE(run("axis status shoulder"));
    EXPECT_NE(output.str().find("encoder: unavailable"), std::string::npos);
}

TEST_F(CliAxis, InvalidMotionFlagsLeaveTheAxesAndDefaultsUntouched)
{
    configure();
    for (const auto command : { "axis move x 1 --speed 1",
                                "axis move shoulder nan --speed 1",
                                "axis move z 1 --speed 0",
                                "axis move z 1 --speed inf",
                                "axis move z 1 --speed 1 --accel -1",
                                "axis move z 1 --speed 1 --decel nan",
                                "axis move z 1 --speed 1 --jerk -1",
                                "axis move z 1 --speed 1 --speed 2",
                                "axis move z 1 --speed 1 --buffer unknown",
                                "axis move shoulder 1 --speed 1 --timeout -1",
                                "axis defaults shoulder --accel 0",
                                "axis defaults z --jerk -1",
                                "axis home z --seek 1 --latch 1",
                                "axis home z --seek 0",
                                "axis home z --timeout 0",
                                "axis home shoulder --latch 6",
                                "axis speed z -1" })
        EXPECT_FALSE(run(command)) << command;
    EXPECT_TRUE(controller->motions().empty());
    EXPECT_FALSE(controller->status().enabled);
}
