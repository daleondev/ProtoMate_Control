// Numeric values describe explicit test inputs, timing bounds and expected results.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
#include "cli/Parser.hpp"
#include "cli/motor.hpp"
#include "control/MotionController.hpp"
#include "control/tests/Connection.hpp"
#include "hal/devices/impl/linux/Tmc2209Model.hpp"
#include "hal/drivers/impl/linux/Gpio.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <sstream>

namespace
{
    using Controller = control::MotionController;
    using namespace pnm::units::literals;
    using namespace std::chrono_literals;
    using enum hal::gpio::Level;

    struct CliMotion : public testing::Test
    {
        std::shared_ptr<hal::device::Tmc2209Model> driver_bus{
            std::make_shared<hal::device::Tmc2209Model>()
        };
        std::shared_ptr<Controller> m_controller;
        std::unique_ptr<control::test::Connection> m_connection;
        cli::Parser m_parser;
        std::ostringstream m_output;
        std::array<std::shared_ptr<hal::GpioInput>, 3> m_switches;

        void SetUp() override
        {
            m_controller = std::make_shared<Controller>(
              std::array<Controller::AxisConfig, 3>{ {
                { .reference_position = 135_deg, .full_step_angle = 1.8_deg, .microsteps = 16U },
                { .reference_position = 135_deg, .full_step_angle = 1.8_deg, .microsteps = 16U },
                { .reference_position = 135_deg, .full_step_angle = 1.8_deg, .microsteps = 16U },
              } },
              [bus = driver_bus](const auto& microsteps, const auto& enable) {
                return hal::board::create_stepper_drivers(microsteps, enable, bus);
            });
            auto alarm = hal::gpio::simulated_input({ .port = hal::gpio::Port::F, .number = 2 });
            ASSERT_TRUE(alarm);
            EXPECT_TRUE(m_controller->driverStatus()[0].status.fault_active);
            EXPECT_TRUE(m_controller->driverStatus()[0].status.fault_latched);
            EXPECT_THROW(m_controller->enable(), std::runtime_error);
            alarm->setSimulatedLevel(hal::gpio::Level::Low); // Healthy, powered DM542T.
            m_controller->initializeDrivers();
            constexpr std::array<std::uint8_t, 3> pins{ 7, 8, 10 };
            for (std::size_t i{}; i < pins.size(); ++i) {
                m_switches[i] = hal::gpio::simulated_input({ .port = hal::gpio::Port::E, .number = pins[i] });
                ASSERT_TRUE(m_switches[i]);
                m_switches[i]->setSimulatedLevel(Low);
            }
            m_connection = std::make_unique<control::test::Connection>(m_controller);
            cli::motor::setup(m_parser, m_connection->client);
        }

        cli::ExecutionResult run(std::string_view command)
        {
            m_output.str({});
            m_output.clear();
            return m_parser.execute(command, m_output);
        }

        template<typename Predicate>
        bool eventually(Predicate predicate)
        {
            const auto deadline{ std::chrono::steady_clock::now() + 2s };
            for (;;) {
                if (predicate())
                    return true;
                std::this_thread::sleep_for(1ms);
                if (!(std::chrono::steady_clock::now() < deadline))
                    break;
            }
            return false;
        }

        bool finished(std::size_t count)
        {
            return eventually([&] {
                const auto motions{ m_controller->motions() };
                return motions.size() == count && std::ranges::all_of(motions, [](const auto& motion) {
                    return motion.result.has_value();
                });
            });
        }
    };
}

TEST_F(CliMotion, StartupIsIdleAndMotionRequiresExplicitEnable)
{
    const auto status{ m_controller->status() };
    EXPECT_FALSE(status.enabled);
    EXPECT_EQ(status.generator.state, hal::step::State::Running);
    EXPECT_EQ(status.generator.pulses, (std::array<hal::step::PulseCount, 3>{}));
    EXPECT_TRUE(m_controller->motions().empty());
    EXPECT_FALSE(run("motor move m1 -90 --speed 10"));
    EXPECT_FALSE(run("motor home m2"));
    EXPECT_FALSE(run("motor speed m3 20"));
    ASSERT_TRUE(run("motor status"));
    EXPECT_NE(m_output.str().find("Drivers: disabled"), std::string::npos);
    EXPECT_NE(m_output.str().find("switch=released"), std::string::npos);
    EXPECT_NE(m_output.str().find("driver INDEX (pseudo): waiting for INDEX"), std::string::npos);
    m_switches[0]->setSimulatedLevel(High);
    ASSERT_TRUE(run("motor status 1"));
    EXPECT_NE(m_output.str().find("switch=active/open"), std::string::npos);
    EXPECT_EQ(m_output.str().find("m2:"), std::string::npos);
    ASSERT_TRUE(run("motor"));
    EXPECT_NE(m_output.str().find("motor moveto"), std::string::npos);
    ASSERT_TRUE(run("help motor move"));
    EXPECT_NE(m_output.str().find("blending-previous"), std::string::npos);
}

TEST_F(CliMotion, RejectsInvalidOptionsWithoutSubmittingOrChangingDefaults)
{
    const auto original{ m_controller->defaults(Controller::MotorId::Motor1) };
    for (const auto* const text : { "motor move m4 10 --speed 1",
                                    "motor move m1 10",
                                    "motor move m1 nan --speed 1",
                                    "motor move m1 10deg --speed 1",
                                    "motor move m1 10 --speed inf",
                                    "motor move m1 10 --speed 0",
                                    "motor move m1 10 --speed -1",
                                    "motor move m1 10 --speed 1 --speed 2",
                                    "motor move m1 10 --speed 1 --accel -1",
                                    "motor move m1 10 --speed 1 --decel nan",
                                    "motor move m1 10 --speed 1 --jerk -1",
                                    "motor move m1 10 --speed 1 --timeout -1",
                                    "motor move m1 10 --speed 1 --buffer nonsense",
                                    "motor defaults m1 --accel 0",
                                    "motor defaults m1 --accel 1 --jerk -1",
                                    "motor home m1 --latch 6",
                                    "motor home m1 --timeout 0",
                                    "motor enable m1",
                                    "motor stop m4",
                                    "motor jobs 0",
                                    "motor jobs -1",
                                    "motor jobs 1.5",
                                    "motor jobs 999999999999999999999999999",
                                    "motor nope",
                                    "motor status all extra" }) {
        const auto result{ run(text) };
        EXPECT_TRUE(!result || result.exit_code != 0) << text;
    }
    EXPECT_TRUE(m_controller->motions().empty());
    EXPECT_EQ(m_controller->defaults(Controller::MotorId::Motor1).acceleration, original.acceleration);
    EXPECT_FALSE(m_controller->status().enabled);
}

TEST_F(CliMotion, NegativeMoveCompletesAndReportsItsResultWithoutBlockingThePrompt)
{
    ASSERT_TRUE(run("motor enable"));
    ASSERT_TRUE(run("motor move m1 -9 --speed 30 --accel 3600 --decel 1800 --jerk 36000 --timeout 2"));
    ASSERT_TRUE(finished(1));
    EXPECT_EQ(m_controller->motions()[0].result, StepperMotor::Result::Completed);
    EXPECT_NEAR(m_controller->status().axes[0].position.get<pnm::units::AngleUnits::deg>(), -9, 1e-9);
    ASSERT_TRUE(run("motor jobs 1"));
    EXPECT_EQ(m_output.str(), "#1 m1 completed\n");
    const auto unreferenced{ run("motor moveto m1 10 --speed 10") };
    EXPECT_FALSE(unreferenced);
    EXPECT_NE(unreferenced.message.find("not referenced"), std::string::npos);
    EXPECT_FALSE(run("motor reset"));
    ASSERT_TRUE(run("motor disable"));
    ASSERT_TRUE(run("motor reset"));
    EXPECT_FALSE(m_controller->status().enabled);
}

TEST_F(CliMotion, SharedClientsControlTheSameAxesAndStoppingOneLeavesTheOtherRunning)
{
    cli::Parser second_client;
    cli::motor::setup(second_client, m_connection->client);
    ASSERT_TRUE(run("motor enable"));
    ASSERT_TRUE(run("motor move m1 -720 --speed 10"));
    ASSERT_TRUE(run("motor move m2 -720 --speed 10"));
    ASSERT_TRUE(eventually([&] {
        const auto status{ m_controller->status() };
        return status.axes[0].velocity < 0_rpm && status.axes[1].velocity < 0_rpm;
    }));
    ASSERT_TRUE(second_client.execute("motor stop 1", m_output));
    const auto status{ m_controller->status() };
    EXPECT_EQ(status.axes[0].velocity, 0_rpm);
    EXPECT_LT(status.axes[1].velocity, 0_rpm);
    EXPECT_TRUE(status.enabled);
    EXPECT_EQ(m_controller->motions()[0].result, StepperMotor::Result::Stopped);
    ASSERT_TRUE(run("motor disable"));
    ASSERT_TRUE(finished(2));
    EXPECT_FALSE(m_controller->status().enabled);
    EXPECT_EQ(m_controller->motions()[1].result, StepperMotor::Result::Stopped);
    const auto stopped{ run("motor jobs 2") };
    EXPECT_NE(stopped.exit_code, 0);
}

TEST_F(CliMotion, DefaultsBufferedCommandsAndLiveSpeedArePassedToTheMotor)
{
    ASSERT_TRUE(run("motor defaults m2 --accel 720 --decel 360 --jerk 0"));
    const auto defaults{ m_controller->defaults(Controller::MotorId::Motor2) };
    EXPECT_EQ(defaults.acceleration, 720_deg_s2);
    EXPECT_EQ(defaults.deceleration, 360_deg_s2);
    EXPECT_EQ(defaults.jerk, 0_deg_s3);
    ASSERT_TRUE(run("motor enable"));
    ASSERT_TRUE(run("motor move m2 -720 --speed 10"));
    ASSERT_TRUE(eventually([&] { return m_controller->status().axes[1].velocity < 0_rpm; }));
    EXPECT_FALSE(run("motor defaults m2 --jerk 1000"));
    ASSERT_TRUE(run("motor move m2 -90 --speed 20 --buffer buffered"));
    const auto full{ run("motor move m2 -45 --speed 20 --buffer buffered") };
    EXPECT_NE(full.exit_code, 0);
    ASSERT_TRUE(run("motor speed m2 20"));
    EXPECT_NE(m_output.str().find("Speed update scheduled"), std::string::npos);
    ASSERT_TRUE(run("motor stop"));
    ASSERT_TRUE(finished(3));
    const auto jobs{ m_controller->motions() };
    EXPECT_EQ(jobs[0].result, StepperMotor::Result::Stopped);
    EXPECT_EQ(jobs[1].result, StepperMotor::Result::Stopped);
    EXPECT_EQ(jobs[2].result, StepperMotor::Result::Rejected);
}

TEST_F(CliMotion, HomingPermitsAbsoluteMovesAndDisablingInvalidatesTheDatum)
{
    ASSERT_TRUE(run("motor enable"));
    ASSERT_TRUE(run("motor home m2 --seek 5 --latch .5 --timeout 2"));
    ASSERT_TRUE(eventually([&] { return m_controller->status().axes[1].velocity > 0_rpm; }));
    m_switches[1]->setSimulatedLevel(High);
    ASSERT_TRUE(eventually([&] { return m_controller->status().axes[1].velocity < 0_rpm; }));
    m_switches[1]->setSimulatedLevel(Low);
    ASSERT_TRUE(eventually([&] { return m_controller->status().axes[1].velocity > 0_rpm; }));
    m_switches[1]->setSimulatedLevel(High);
    ASSERT_TRUE(finished(1));
    EXPECT_EQ(m_controller->motions()[0].result, StepperMotor::Result::Completed);
    EXPECT_TRUE(m_controller->status().axes[1].referenced);
    EXPECT_EQ(m_controller->status().axes[1].position, 135_deg);
    ASSERT_TRUE(run("motor moveto m2 134.1 --speed 10"));
    ASSERT_TRUE(finished(2));
    EXPECT_EQ(m_controller->motions()[1].result, StepperMotor::Result::Completed);
    EXPECT_NEAR(m_controller->status().axes[1].position.get<pnm::units::AngleUnits::deg>(), 134.1, 1e-9);
    ASSERT_TRUE(run("motor disable"));
    EXPECT_FALSE(m_controller->status().axes[1].referenced);
    ASSERT_TRUE(run("motor enable"));
    EXPECT_FALSE(run("motor moveto m2 130 --speed 10"));
}

TEST_F(CliMotion, ResultsStayBoundedAndAllBufferModesAreAccepted)
{
    ASSERT_TRUE(run("motor enable"));
    const auto retained{ m_connection->client->move(Controller::MotorId::Motor3,
                                                    { .position = 0_deg, .velocity = 10_rpm }) };
    ASSERT_TRUE(retained.completion.valid());
    ASSERT_EQ(retained.completion.wait_for(1s), std::future_status::ready);
    for (const auto* const mode :
         { "aborting", "buffered", "blending-low", "blending-previous", "blending-next", "blending-high" }) {
        const auto result{ run(std::string("motor move m3 0 --speed 10 --buffer ") + mode) };
        ASSERT_TRUE(result) << result.message;
        ASSERT_TRUE(eventually([&] { return m_controller->motions().back().result.has_value(); }));
        EXPECT_EQ(m_controller->motions().back().result, StepperMotor::Result::Completed);
    }
    for (int i{}; i < 35; ++i) {
        ASSERT_TRUE(run("motor move m3 0 --speed 10"));
        ASSERT_TRUE(eventually([&] { return m_controller->motions().back().result.has_value(); }));
    }
    EXPECT_EQ(m_controller->motions().size(), 32U);
    EXPECT_EQ(m_controller->motions().back().id, 42U);
    EXPECT_EQ(retained.completion.get(), StepperMotor::Result::Completed);
    EXPECT_FALSE(run("motor jobs 1"));
    ASSERT_TRUE(run("motor jobs 42"));
    EXPECT_EQ(m_output.str(), "#42 m3 completed\n");
}

TEST_F(CliMotion, DriverCommandsInitializeConfigureAndEnforceMotorLimits)
{
    ASSERT_TRUE(run("motor driver status"));
    EXPECT_NE(m_output.str().find("UART address=0"), std::string::npos);
    EXPECT_NE(m_output.str().find("UART address=1"), std::string::npos);
    ASSERT_TRUE(run("motor driver init"));
    ASSERT_TRUE(run("motor driver configure m2 --run 600 --hold 400 --mode spreadcycle --interpolate off"));
    ASSERT_TRUE(run("motor driver configure m3 --run 550 --mode stealthchop"));
    const auto driver_status{ m_controller->driverStatus() };
    const auto driver_configuration{ driver_status[2].configuration };
    if (!driver_configuration) {
        FAIL() << "expected driver_status[2].configuration to have a value";
    }
    EXPECT_EQ(driver_configuration->configuration.hold_milliamps, 550);
    for (const auto* command : { "motor driver configure m1 --run 500",
                                 "motor driver configure m2 --run 701",
                                 "motor driver configure m3 --run 591",
                                 "motor driver configure m3 --hold 400",
                                 "motor driver configure m2 --run -1",
                                 "motor driver configure m2 --run 500.5",
                                 "motor driver configure m2 --run nan",
                                 "motor driver configure m2 --mode unknown",
                                 "motor driver configure m2 --interpolate yes",
                                 "motor driver configure m2 --run 100 --hold 100 --mode stealthchop" })
        EXPECT_FALSE(run(command)) << command;
    ASSERT_TRUE(run("motor enable"));
    EXPECT_FALSE(run("motor driver init"));
    EXPECT_FALSE(run("motor driver configure m2 --run 600"));
    ASSERT_TRUE(run("motor disable"));
}

TEST_F(CliMotion, DriverResetOrLostCommunicationLatchesFaultUntilExplicitRecovery)
{
    const auto& bus = driver_bus;
    ASSERT_TRUE(run("motor enable"));
    ASSERT_TRUE(run("motor move m2 -360 --speed 1"));
    bus->reset(0);
    ASSERT_TRUE(eventually([&] { return !m_controller->status().enabled; }));
    EXPECT_FALSE(run("motor enable"));
    EXPECT_FALSE(run("motor move m1 -10 --speed 1"));
    ASSERT_TRUE(run("motor driver init"));
    ASSERT_TRUE(run("motor enable"));
    bus->setConnected(1, false);
    ASSERT_TRUE(eventually([&] { return !m_controller->status().enabled; }));
    EXPECT_FALSE(run("motor driver init"));
    EXPECT_FALSE(run("motor enable"));
    bus->setConnected(1, true);
    ASSERT_TRUE(run("motor driver init"));
    EXPECT_FALSE(m_controller->status().enabled);
    ASSERT_TRUE(run("motor enable"));
}

TEST_F(CliMotion, DiagnosticEdgeDisablesAllDriversAndStopsMotion)
{
    auto diag = hal::gpio::simulated_input({ .port = hal::gpio::Port::D, .number = 3 });
    ASSERT_TRUE(diag);
    ASSERT_TRUE(run("motor enable"));
    ASSERT_TRUE(run("motor move m1 -360 --speed 1"));
    diag->setSimulatedLevel(High);
    EXPECT_FALSE(m_controller->status().enabled);
    ASSERT_TRUE(finished(1));
    EXPECT_FALSE(run("motor enable"));
    EXPECT_FALSE(run("motor driver init"));
    diag->setSimulatedLevel(Low);
    ASSERT_TRUE(run("motor driver init"));
    EXPECT_FALSE(m_controller->status().enabled);
}

TEST_F(CliMotion, Dm542AlarmStopsAllAxesAndRequiresExplicitRecoveryAfterTheSignalClears)
{
    auto alarm = hal::gpio::simulated_input({ .port = hal::gpio::Port::F, .number = 2 });
    ASSERT_TRUE(run("motor driver status m1"));
    EXPECT_NE(m_output.str().find("fault-input=clear fault-latched=false"), std::string::npos);
    ASSERT_TRUE(run("motor enable"));
    ASSERT_TRUE(run("motor move m1 -360 --speed 1"));
    ASSERT_TRUE(run("motor move m2 -360 --speed 1"));
    alarm->setSimulatedLevel(High);
    EXPECT_FALSE(m_controller->status().enabled);
    EXPECT_TRUE(m_controller->driverStatus()[0].status.fault_latched);
    ASSERT_TRUE(finished(2));
    EXPECT_FALSE(run("motor enable"));
    EXPECT_FALSE(run("motor reset"));
    EXPECT_FALSE(run("motor driver init"));
    ASSERT_TRUE(run("motor driver status"));
    EXPECT_NE(m_output.str().find("fault-input=active/open fault-latched=true"), std::string::npos);
    alarm->setSimulatedLevel(Low);
    EXPECT_FALSE(m_controller->driverStatus()[0].status.fault_active);
    EXPECT_TRUE(m_controller->driverStatus()[0].status.fault_latched);
    EXPECT_FALSE(run("motor enable"));
    EXPECT_FALSE(run("motor move m3 -10 --speed 1"));
    ASSERT_TRUE(run("motor reset"));
    EXPECT_FALSE(m_controller->driverStatus()[0].status.fault_latched);
    EXPECT_FALSE(m_controller->status().enabled);
    ASSERT_TRUE(run("motor enable"));
}

TEST_F(CliMotion, Dm542AlarmIsLatchedEvenWhenAUartDriverIsUnavailable)
{
    const auto& bus = driver_bus;
    bus->setConnected(1, false);
    EXPECT_FALSE(run("motor driver init"));
    auto alarm = hal::gpio::simulated_input({ .port = hal::gpio::Port::F, .number = 2 });
    alarm->setSimulatedLevel(High);
    alarm->setSimulatedLevel(Low); // A short alarm must not be lost between polls.
    EXPECT_TRUE(m_controller->driverStatus()[0].status.fault_latched);
    EXPECT_FALSE(run("motor enable"));
    bus->setConnected(1, true);
    ASSERT_TRUE(run("motor driver init"));
    EXPECT_FALSE(m_controller->driverStatus()[0].status.fault_latched);
}

TEST_F(CliMotion, Dm542AlarmDuringEnableSettlingCannotLeaveDriversEnabled)
{
    auto alarm = hal::gpio::simulated_input({ .port = hal::gpio::Port::F, .number = 2 });
    std::jthread fault([alarm] {
        std::this_thread::sleep_for(50ms);
        alarm->setSimulatedLevel(High);
    });
    EXPECT_FALSE(run("motor enable"));
    EXPECT_FALSE(m_controller->status().enabled);
    EXPECT_TRUE(m_controller->driverStatus()[0].status.fault_latched);
}

TEST_F(CliMotion, Dm542AlarmInvalidatesAReferencedAxis)
{
    ASSERT_TRUE(run("motor enable"));
    ASSERT_TRUE(run("motor home m2 --seek 5 --latch .5 --timeout 2"));
    ASSERT_TRUE(eventually([&] { return m_controller->status().axes[1].velocity > 0_rpm; }));
    m_switches[1]->setSimulatedLevel(High);
    ASSERT_TRUE(eventually([&] { return m_controller->status().axes[1].velocity < 0_rpm; }));
    m_switches[1]->setSimulatedLevel(Low);
    ASSERT_TRUE(eventually([&] { return m_controller->status().axes[1].velocity > 0_rpm; }));
    m_switches[1]->setSimulatedLevel(High);
    ASSERT_TRUE(finished(1));
    ASSERT_TRUE(m_controller->status().axes[1].referenced);
    auto alarm = hal::gpio::simulated_input({ .port = hal::gpio::Port::F, .number = 2 });
    alarm->setSimulatedLevel(High);
    ASSERT_TRUE(eventually([&] { return !m_controller->status().axes[1].referenced; }));
    EXPECT_FALSE(m_controller->status().enabled);
    alarm->setSimulatedLevel(Low);
    ASSERT_TRUE(run("motor driver init"));
    ASSERT_TRUE(run("motor enable"));
    EXPECT_FALSE(run("motor moveto m2 130 --speed 10"));
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
