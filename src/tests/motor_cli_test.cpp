#include "common/icc/bridge/Bridge.hpp"
#include "common/icc/catalog/actions.hpp"
#include "common/icc/catalog/services.hpp"
#include "common/icc/examples/ContextThread.hpp"
#include "common/icc/transport/InProcessTransport.hpp"
#include "hal/drivers/impl/linux/Gpio.hpp"
#include "io/motion/Server.hpp"
#include "ui/motion/Client.hpp"
#include "ui/cli/Parser.hpp"
#include "ui/cli/motor.hpp"

#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <sstream>

namespace
{
    using namespace std::chrono_literals;
    using namespace pnm::units::literals;
    using Controller = io::motion::Controller;
    using Id = Controller::MotorId;
    using State = ui::motion::Client::JobState;
    using namespace motion;
    constexpr Controller::Configuration CONFIGURATION{
        Controller::MotorConfiguration{ 135_deg, 1.8_deg, 16 },
        Controller::MotorConfiguration{ 135_deg, 1.8_deg, 16 },
        Controller::MotorConfiguration{ 135_deg, 1.8_deg, 16 }
    };

    class Driver final : public hal::device::IStepperDriver
    {
      public:
        std::atomic_bool initialized{};
        std::string_view name() const noexcept override { return "test driver"; }
        Status status() const noexcept override { return { .ready = initialized.load(), .error = {} }; }
        hal::util::Result<> initialize() override { initialized = true; return {}; }
        hal::util::Result<> verify() override { return {}; }
        void service() override {}
        void setFaultCallback(FaultCallback) override {}
    };

    template<typename Predicate> bool waitUntil(Predicate predicate)
    {
        const auto end{ std::chrono::steady_clock::now() + 3s };
        while (!predicate() && std::chrono::steady_clock::now() < end) { std::this_thread::sleep_for(1ms); }
        return predicate();
    }

    class MotorCli : public ::testing::Test
    {
      protected:
        pnm::msg::Bus ioBus, uiBus;
        pnm::coro::Context ioContext, uiContext;
        decltype(icc::InProcessTransport::pair()) endpoints{ icc::InProcessTransport::pair() };
        std::jthread ioRunner{ example::run_context(ioContext) }, uiRunner{ example::run_context(uiContext) };
        icc::Bridge ioBridge{ ioBus, ioContext, *endpoints[0] }, uiBridge{ uiBus, uiContext, *endpoints[1] };
        Controller controller{ CONFIGURATION, [](const auto&, const auto&) {
            return Controller::Drivers{ std::make_shared<Driver>(), std::make_shared<Driver>(), std::make_shared<Driver>() };
        } };
        io::motion::Server server{ ioBus, controller };
        ui::motion::Client client{ uiBus };
        ui::cli::Parser parser;
        std::stop_source ioStop, uiStop;
        std::jthread ioWorker, uiWorker;
        std::ostringstream output;

        void SetUp() override
        {
            for (auto pin : std::array<std::uint8_t, 3>{ 7, 8, 10 }) {
                hal::gpio::simulated_input({ hal::gpio::Port::E, pin })->setSimulatedLevel(hal::gpio::Level::Low);
            }
            ioBridge.registerServices<icc::Services>(icc::ServiceProvider::IO);
            uiBridge.registerServices<icc::Services>(icc::ServiceProvider::UI);
            ioBridge.registerActions<icc::Actions>(icc::ActionProvider::IO);
            uiBridge.registerActions<icc::Actions>(icc::ActionProvider::UI);
            ioBridge.setConnectionCallback([this](icc::ConnectionStatus value) {
                if (value.state == icc::ConnectionState::Failed || value.reason == icc::ConnectionReason::PeerStopped) {
                    ioStop.request_stop();
                }
            });
            ui::cli::motor::setup(parser, client, uiStop.get_token());
            ioWorker = std::jthread([this] { server.run(ioStop.get_token()); });
            uiWorker = std::jthread([this] { client.run(uiStop.get_token()); });
            ioBridge.start();
            uiBridge.start();
        }
        void TearDown() override
        {
            uiStop.request_stop();
            ioStop.request_stop();
            uiWorker.join();
            ioWorker.join();
            ioBridge.stop();
            uiBridge.stop();
        }
        auto command(std::string_view text)
        {
            output.str({}); output.clear();
            return parser.execute(text, output);
        }
        void enable()
        {
            ASSERT_TRUE(command("motor driver init")) << output.str();
            ASSERT_TRUE(command("motor enable")) << output.str();
        }
        bool jobIs(std::uint64_t id, State state)
        {
            const auto jobs{ client.jobs() };
            for (const auto& job : jobs) { if (job.id == id) { return job.state == state; } }
            return false;
        }
        double position(Id id) { return controller.motor(id).setpointPosition().get<pnm::units::AngleUnits::deg>(); }
        auto rejected(MotorGoal goal)
        {
            std::optional<pnm::msg::ActionResult<MotorResult>> result;
            auto call{ uiBus.action<^^icc::Actions::io_motor_motion>().sendGoal(goal, { .accept_timeout = 2s },
                { .on_result = [&result](auto value) { result = value; } }) };
            EXPECT_TRUE(waitUntil([&] { return call.poll(); }));
            EXPECT_TRUE(result.has_value());
            return result;
        }
    };

    TEST_F(MotorCli, RegistrationHelpValidationAndDisabledAdmission)
    {
        ASSERT_NO_THROW(ui::cli::motor::setup(parser, client));
        ui::cli::Parser second;
        ui::cli::motor::setup(second, client);
        EXPECT_TRUE(second.contains("motor move"));
        EXPECT_FALSE(second.contains("axis"));
        EXPECT_FALSE(second.contains("robot"));
        ASSERT_TRUE(command("help motor move"));
        EXPECT_NE(output.str().find("blending-high"), std::string::npos);
        for (auto text : { "motor move m1 1", "motor move m4 1 --speed 5", "motor move m1 nan --speed 5",
                           "motor move m1 1 --speed inf", "motor move m1 1 --speed 0", "motor move m1 1 --speed 5 --accel -1",
                           "motor move m1 1 --speed 5 --speed 6", "motor move m1 1 --speed 5 --buffer nonsense",
                           "motor home m1 --latch 6", "motor home m1 --timeout 0", "motor jobs 1.5" }) {
            EXPECT_FALSE(command(text)) << text;
        }
        EXPECT_TRUE(client.jobs().empty());
        EXPECT_FALSE(command("motor move m1 1 --speed 5"));
        ASSERT_TRUE(command("motor status"));
        EXPECT_NE(output.str().find("not ready"), std::string::npos);
        EXPECT_NE(output.str().find("driver INDEX (pseudo)"), std::string::npos);
        ASSERT_TRUE(command("motor driver status m2"));
        EXPECT_NE(output.str().find("m2 test driver"), std::string::npos);
        EXPECT_EQ(output.str().find("m1"), std::string::npos);
    }

    TEST_F(MotorCli, MoveCompletesAndJobOutlivesCommand)
    {
        enable();
        ASSERT_TRUE(command("motor move m1 -1.125 --speed 5"));
        EXPECT_NE(output.str().find("Submitted motor job 1"), std::string::npos);
        ASSERT_TRUE(waitUntil([&] { return jobIs(1, State::Completed); }));
        EXPECT_NEAR(position(Id::Motor1), -1.125, 0.0001);
        ASSERT_TRUE(command("motor jobs 1"));
        EXPECT_NE(output.str().find("completed"), std::string::npos);
        ASSERT_TRUE(command("motor moveto m1 0 --speed 5"));
        ASSERT_TRUE(waitUntil([&] { return jobIs(2, State::Rejected); }));
        EXPECT_NEAR(position(Id::Motor1), -1.125, 0.0001);
    }

    TEST_F(MotorCli, StopIsIndependentAndDisableStopsAll)
    {
        enable();
        ASSERT_TRUE(command("motor move m1 -90 --speed 5"));
        ASSERT_TRUE(command("motor move m2 -90 --speed 5"));
        ASSERT_TRUE(waitUntil([&] { return position(Id::Motor1) < -1 && position(Id::Motor2) < -1; }));
        ASSERT_TRUE(command("motor stop m1"));
        ASSERT_TRUE(waitUntil([&] { return jobIs(1, State::Stopped); }));
        EXPECT_TRUE(jobIs(2, State::Accepted));
        EXPECT_TRUE(controller.enabled());
        const double stopped{ position(Id::Motor1) }, moving{ position(Id::Motor2) };
        ASSERT_TRUE(waitUntil([&] { return position(Id::Motor2) < moving - 1; }));
        EXPECT_EQ(position(Id::Motor1), stopped);
        ASSERT_TRUE(command("motor disable"));
        ASSERT_TRUE(waitUntil([&] { return jobIs(2, State::Stopped); }));
        EXPECT_FALSE(controller.enabled());
    }

    TEST_F(MotorCli, BufferedMovesAndRuntimeSpeedUpdate)
    {
        enable();
        ASSERT_TRUE(command("motor move m2 -90 --speed 5"));
        ASSERT_TRUE(waitUntil([&] { return position(Id::Motor2) < 0; }));
        const auto speed{ command("motor speed m2 10") };
        ASSERT_TRUE(speed) << speed.message;
        EXPECT_NE(output.str().find("Speed update scheduled"), std::string::npos);
        ASSERT_TRUE(command("motor move m2 -9 --speed 5 --buffer buffered"));
        ASSERT_TRUE(waitUntil([&] { return jobIs(1, State::Completed) && jobIs(2, State::Completed); }));
        EXPECT_NEAR(position(Id::Motor2), -99, 0.0001);
        ASSERT_TRUE(command("motor defaults m2 --accel 1200 --decel 900 --jerk 0"));
        EXPECT_NE(output.str().find("accel=1200"), std::string::npos);
        EXPECT_EQ(controller.motor(Id::Motor2).motionDefaults().jerk, 0_deg_s3);
        EXPECT_FALSE(command("motor defaults m2 --accel 0"));
    }

    TEST_F(MotorCli, HomingStopHomingAndTimeoutRemainUsable)
    {
        enable();
        for (std::uint64_t attempt = 1; attempt <= 2; ++attempt) {
            ASSERT_TRUE(command("motor home m1"));
            ASSERT_TRUE(waitUntil([&] { return controller.motor(Id::Motor1).setpointVelocity() > 0_rpm; }));
            ASSERT_TRUE(command("motor stop m1"));
            ASSERT_TRUE(waitUntil([&] { return jobIs(attempt, State::Stopped); }));
            EXPECT_FALSE(controller.motor(Id::Motor1).isReferenced());
        }
        ASSERT_TRUE(command("motor home m1 --timeout 0.05"));
        ASSERT_TRUE(waitUntil([&] { return jobIs(3, State::TimedOut); }));
        EXPECT_TRUE(controller.enabled());
    }

    TEST_F(MotorCli, StaleAndMalformedGoalsCannotMoveHardware)
    {
        enable();
        const auto status{ client.request({}) };
        MotorGoal goal{ .motor = MotorId::M1, .position = -10, .velocity = 5, .generation = status.generation };
        ASSERT_TRUE(command("motor stop"));
        auto result{ rejected(goal) };
        ASSERT_TRUE(result);
        EXPECT_EQ(result->error(), pnm::msg::ActionError::Rejected);
        goal.generation = client.request({}).generation;
        goal.velocity = std::numeric_limits<double>::quiet_NaN();
        result = rejected(goal);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->error(), pnm::msg::ActionError::Rejected);
        goal.velocity = 5;
        goal.motor = static_cast<MotorId>(99);
        result = rejected(goal);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->error(), pnm::msg::ActionError::Rejected);
        EXPECT_EQ(position(Id::Motor1), 0);
    }

    TEST_F(MotorCli, ActionCancellationConfirmsStopAndLeavesOtherMotorRunning)
    {
        enable();
        ASSERT_TRUE(command("motor move m2 -90 --speed 5"));
        const auto status{ client.request({}) };
        std::optional<pnm::msg::ActionResult<MotorResult>> result;
        auto action{ uiBus.action<^^icc::Actions::io_motor_motion>().sendGoal(
            { .motor = MotorId::M1, .position = -90, .velocity = 5, .generation = status.generation },
            { .accept_timeout = 2s }, { .on_result = [&result](auto value) { result = value; } }) };
        ASSERT_TRUE(waitUntil([&] { action.poll(); return position(Id::Motor1) < -1; }));
        action.requestCancel();
        ASSERT_TRUE(waitUntil([&] { return action.poll(); }));
        ASSERT_TRUE(result && *result);
        EXPECT_EQ((*result)->status, pnm::msg::ActionStatus::Cancelled);
        EXPECT_EQ((*result)->value.outcome, MotorOutcome::Stopped);
        EXPECT_EQ(controller.motor(Id::Motor1).setpointVelocity(), 0_rpm);
        EXPECT_TRUE(controller.enabled());
        const double before{ position(Id::Motor2) };
        ASSERT_TRUE(waitUntil([&] { return position(Id::Motor2) < before - 1; }));
    }

    TEST_F(MotorCli, HomingCompletesBeforeAbsoluteMovesAreAllowed)
    {
        enable();
        auto input{ hal::gpio::simulated_input({ hal::gpio::Port::E, 7 }) };
        ASSERT_TRUE(command("motor home m1"));
        ASSERT_TRUE(waitUntil([&] { return controller.motor(Id::Motor1).setpointVelocity() > 0_rpm; }));
        input->setSimulatedLevel(hal::gpio::Level::High);
        ASSERT_TRUE(waitUntil([&] { return controller.motor(Id::Motor1).setpointVelocity() < 0_rpm; }));
        input->setSimulatedLevel(hal::gpio::Level::Low);
        ASSERT_TRUE(waitUntil([&] { return controller.motor(Id::Motor1).setpointVelocity() > 0_rpm; }));
        input->setSimulatedLevel(hal::gpio::Level::High);
        ASSERT_TRUE(waitUntil([&] { return jobIs(1, State::Completed); }));
        EXPECT_TRUE(controller.motor(Id::Motor1).isReferenced());
        EXPECT_NEAR(position(Id::Motor1), 135, 0.0001);
        // Moving away from an active reference input is permitted.
        ASSERT_TRUE(command("motor moveto m1 130.5 --speed 5"));
        ASSERT_TRUE(waitUntil([&] { return jobIs(2, State::Completed); }));
        EXPECT_NEAR(position(Id::Motor1), 130.5, 0.0001);
    }

    TEST_F(MotorCli, PeerShutdownStopsMotionAndDisablesDrivers)
    {
        enable();
        ASSERT_TRUE(command("motor move m3 -90 --speed 5"));
        ASSERT_TRUE(waitUntil([&] { return position(Id::Motor3) < -1; }));
        uiBridge.stop();
        ASSERT_TRUE(waitUntil([&] { return !controller.enabled(); }));
        EXPECT_FALSE(controller.motor(Id::Motor3).isReferenced());
        ASSERT_TRUE(waitUntil([&] { return jobIs(1, State::Unknown) || jobIs(1, State::Stopped); }))
            << static_cast<int>(client.jobs().front().state);
        EXPECT_FALSE(command("motor status"));
    }

    TEST_F(MotorCli, HistoryIsBoundedAndFailedStopIsNotReportedAsSuccess)
    {
        enable();
        for (std::uint64_t i = 1; i <= 34; ++i) {
            ASSERT_TRUE(command("motor move m1 0 --speed 5"));
            ASSERT_TRUE(waitUntil([&] { return jobIs(i, State::Completed); }));
        }
        EXPECT_EQ(client.jobs().size(), 32U);
        EXPECT_FALSE(command("motor jobs 1"));
        ioStop.request_stop();
        ioWorker.join();
        EXPECT_FALSE(command("motor stop"));
        EXPECT_TRUE(output.str().empty());
        // TearDown normally joins this thread.
        ioWorker = std::jthread([] {});
    }
}
