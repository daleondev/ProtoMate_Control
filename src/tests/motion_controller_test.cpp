#include "io/motion/Controller.hpp"
#include "hal/drivers/impl/linux/Gpio.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <type_traits>

namespace
{
    using namespace pnm::units::literals;
    using namespace std::chrono_literals;
    using Controller = io::motion::Controller;
    using Motor = io::motion::StepperMotor;
    using Id = Controller::MotorId;
    using Level = hal::gpio::Level;
    constexpr Controller::Configuration CONFIGURATION{
        Controller::MotorConfiguration{ 135_deg, 1.8_deg, 16 },
        Controller::MotorConfiguration{ 135_deg, 1.8_deg, 16 },
        Controller::MotorConfiguration{ 0_deg, 1.8_deg, 16 }
    };

    class Driver final : public hal::device::IStepperDriver
    {
      public:
        std::shared_ptr<hal::IDigitalOutput> enable;
        bool fail_initialization{};
        bool fail_verification{};
        std::atomic_uint service_calls{};

        std::string_view name() const noexcept override { return "controller test driver"; }
        Status status() const noexcept override
        {
            std::scoped_lock lock(m_mutex);
            return m_status;
        }
        hal::util::Result<> initialize() override
        {
            std::scoped_lock lock(m_mutex);
            if (enable->read() != Level::High || fail_initialization) {
                return std::unexpected(std::make_error_code(std::errc::io_error));
            }
            m_status = { .ready = true, .error = {} };
            return {};
        }
        hal::util::Result<> verify() override
        {
            if (fail_verification || !status().ready) {
                return std::unexpected(std::make_error_code(std::errc::io_error));
            }
            return {};
        }
        void service() override { ++service_calls; }
        void setFaultCallback(FaultCallback callback) override
        {
            std::scoped_lock lock(m_mutex);
            m_callback = std::move(callback);
            if (m_status.fault_latched && m_callback) {
                m_callback();
            }
        }
        void fault()
        {
            std::scoped_lock lock(m_mutex);
            m_status = { .fault_latched = true, .fault = Fault::Electrical,
                         .error = std::make_error_code(std::errc::io_error) };
            if (m_callback) {
                m_callback();
            }
        }

      private:
        mutable std::mutex m_mutex;
        Status m_status;
        FaultCallback m_callback;
    };

    struct Hardware
    {
        std::array<std::shared_ptr<Driver>, 3> drivers{
            std::make_shared<Driver>(), std::make_shared<Driver>(), std::make_shared<Driver>() };
        Controller::DriverFactory factory()
        {
            return [this](const std::array<std::size_t, 3>& microsteps,
                          const std::shared_ptr<hal::IDigitalOutput>& enable) {
                EXPECT_EQ(microsteps, (std::array<std::size_t, 3>{ 16, 16, 16 }));
                EXPECT_EQ(enable->read(), Level::High);
                for (auto& driver : drivers) {
                    driver->enable = enable;
                }
                return Controller::Drivers{ drivers[0], drivers[1], drivers[2] };
            };
        }
        Level enableLevel() const { return drivers[0]->enable->read(); }
    };

    template<typename Predicate>
    bool waitUntil(Predicate predicate)
    {
        const auto deadline{ std::chrono::steady_clock::now() + 2s };
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        return predicate();
    }

    void releaseSwitches()
    {
        for (auto pin : std::array<std::uint8_t, 3>{ 7, 8, 10 }) {
            auto input{ hal::gpio::simulated_input({ hal::gpio::Port::E, pin }) };
            ASSERT_NE(input, nullptr);
            input->setSimulatedLevel(Level::Low);
        }
    }

    TEST(MotionController, OwnsThreeMotorsAndRejectsMotionUntilInitializedAndEnabled)
    {
        Hardware hardware;
        Controller controller{ CONFIGURATION, hardware.factory() };
        static_assert(std::is_same_v<decltype(controller.motor(Id::Motor1)), const Motor&>);
        static_assert(std::is_same_v<decltype(controller.motor(Id::Motor1).driver()),
                                     const hal::device::IStepperDriver&>);
        EXPECT_EQ(hardware.enableLevel(), Level::High);
        EXPECT_FALSE(controller.ready());
        EXPECT_FALSE(controller.enabled());
        EXPECT_THROW(controller.enable(), std::runtime_error);
        EXPECT_EQ(controller.moveRel(Id::Motor1, -1_deg, 1_rpm).get(), Motor::Result::Rejected);
        EXPECT_EQ(controller.reference(Id::Motor2).get(), Motor::Result::Rejected);
        EXPECT_NE(&controller.motor(Id::Motor1), &controller.motor(Id::Motor2));
        EXPECT_NE(&controller.motor(Id::Motor2), &controller.motor(Id::Motor3));
        EXPECT_EQ(controller.motor(Id::Motor1).feedbackSource(), Motor::FeedbackSource::ShaftEncoder);
        EXPECT_THROW(static_cast<void>(controller.motor(static_cast<Id>(255))), std::invalid_argument);
        controller.initializeDrivers();
        EXPECT_TRUE(controller.ready());
        EXPECT_FALSE(controller.enabled());
        EXPECT_EQ(controller.moveRel(Id::Motor3, -1_deg, 1_rpm).get(), Motor::Result::Rejected);
        controller.enable();
        EXPECT_TRUE(controller.enabled());
        EXPECT_THROW(controller.initializeDrivers(), std::logic_error);
        EXPECT_EQ(controller.moveAbs(Id::Motor1, 10_deg, 1_rpm).get(), Motor::Result::Rejected);
        controller.disable();
        EXPECT_FALSE(controller.enabled());
        EXPECT_EQ(hardware.enableLevel(), Level::High);
    }

    TEST(MotionController, FailedInitializationOrVerificationNeverEnablesAndRequiresRecovery)
    {
        Hardware hardware;
        Controller controller{ CONFIGURATION, hardware.factory() };
        hardware.drivers[1]->fail_initialization = true;
        EXPECT_THROW(controller.initializeDrivers(), std::runtime_error);
        EXPECT_FALSE(controller.ready());
        EXPECT_THROW(controller.enable(), std::runtime_error);
        EXPECT_EQ(hardware.enableLevel(), Level::High);
        hardware.drivers[1]->fail_initialization = false;
        controller.initializeDrivers();
        hardware.drivers[2]->fail_verification = true;
        EXPECT_THROW(controller.enable(), std::runtime_error);
        EXPECT_FALSE(controller.ready());
        EXPECT_EQ(hardware.enableLevel(), Level::High);
        hardware.drivers[2]->fail_verification = false;
        EXPECT_THROW(controller.enable(), std::runtime_error);
        controller.initializeDrivers();
        controller.enable();
        EXPECT_TRUE(controller.enabled());
    }

    TEST(MotionController, IndividualStopLeavesAnotherAxisRunningAndGlobalStopKeepsTorque)
    {
        Hardware hardware;
        Controller controller{ CONFIGURATION, hardware.factory() };
        releaseSwitches();
        controller.initializeDrivers();
        controller.enable();
        auto first{ controller.moveRel(Id::Motor1, -90_deg, 5_rpm) };
        auto second{ controller.moveRel(Id::Motor2, -90_deg, 5_rpm) };
        ASSERT_TRUE(waitUntil([&] {
            return controller.motor(Id::Motor1).setpointPosition() < 0_deg &&
                   controller.motor(Id::Motor2).setpointPosition() < 0_deg;
        }));
        controller.stop(Id::Motor1);
        ASSERT_EQ(first.wait_for(1s), std::future_status::ready);
        EXPECT_EQ(first.get(), Motor::Result::Stopped);
        EXPECT_EQ(second.wait_for(0s), std::future_status::timeout);
        EXPECT_TRUE(controller.enabled());
        controller.stop();
        ASSERT_EQ(second.wait_for(1s), std::future_status::ready);
        EXPECT_EQ(second.get(), Motor::Result::Stopped);
        EXPECT_EQ(hardware.enableLevel(), Level::Low);
        // The shared generator remains usable after an ordinary stop.
        auto third{ controller.moveRel(Id::Motor3, -1.125_deg, 5_rpm) };
        ASSERT_EQ(third.wait_for(2s), std::future_status::ready);
        EXPECT_EQ(third.get(), Motor::Result::Completed);
        EXPECT_NEAR(controller.motor(Id::Motor3).setpointPosition().get<pnm::units::AngleUnits::deg>(),
                    -1.125, 0.0001);
    }

    TEST(MotionController, FaultDisablesAllMotorsCancelsPendingWorkAndDoesNotAutoRecover)
    {
        Hardware hardware;
        Controller controller{ CONFIGURATION, hardware.factory() };
        releaseSwitches();
        controller.initializeDrivers();
        controller.enable();
        auto active{ controller.moveRel(Id::Motor1, -90_deg, 5_rpm) };
        auto pending{ controller.moveRel(Id::Motor1, -90_deg, 5_rpm, 0_rad_s2, 0_rad_s2, 0_rad_s3,
                                         io::motion::BufferMode::Buffered) };
        ASSERT_TRUE(waitUntil([&] { return controller.motor(Id::Motor1).setpointPosition() < 0_deg; }));
        hardware.drivers[2]->fault();
        EXPECT_EQ(hardware.enableLevel(), Level::High); // Synchronous fault path.
        ASSERT_EQ(active.wait_for(2s), std::future_status::ready);
        EXPECT_NE(active.get(), Motor::Result::Completed);
        ASSERT_EQ(pending.wait_for(2s), std::future_status::ready);
        EXPECT_NE(pending.get(), Motor::Result::Completed);
        EXPECT_FALSE(controller.ready());
        EXPECT_EQ(controller.moveRel(Id::Motor2, -1_deg, 5_rpm).get(), Motor::Result::Rejected);
        EXPECT_THROW(controller.enable(), std::runtime_error);
        controller.initializeDrivers();
        EXPECT_TRUE(controller.ready());
        EXPECT_FALSE(controller.enabled());
        controller.enable();
        EXPECT_TRUE(controller.enabled());
        ASSERT_TRUE(waitUntil([&] { return hardware.drivers[0]->service_calls.load() > 0; }));
    }

    TEST(MotionController, HomingCanBeStoppedAndRestarted)
    {
        Hardware hardware;
        Controller controller{ CONFIGURATION, hardware.factory() };
        releaseSwitches();
        controller.initializeDrivers();
        controller.enable();
        for (unsigned attempt{}; attempt < 2; ++attempt) {
            auto homing{ controller.reference(Id::Motor1) };
            ASSERT_TRUE(waitUntil([&] { return controller.motor(Id::Motor1).setpointVelocity() > 0_rpm; }));
            controller.stop(Id::Motor1);
            ASSERT_EQ(homing.wait_for(1s), std::future_status::ready);
            EXPECT_EQ(homing.get(), Motor::Result::Stopped);
            EXPECT_FALSE(controller.motor(Id::Motor1).isReferenced());
            EXPECT_TRUE(controller.enabled());
        }
    }

    TEST(MotionController, DestructionDrainsMotionAndDetachesCallbacks)
    {
        Hardware hardware;
        std::future<Motor::Result> motion;
        {
            Controller controller{ CONFIGURATION, hardware.factory() };
            releaseSwitches();
            controller.initializeDrivers();
            controller.enable();
            motion = controller.moveRel(Id::Motor2, -90_deg, 5_rpm);
            ASSERT_TRUE(waitUntil([&] { return controller.motor(Id::Motor2).setpointPosition() < 0_deg; }));
        }
        EXPECT_EQ(hardware.enableLevel(), Level::High);
        ASSERT_EQ(motion.wait_for(0s), std::future_status::ready);
        EXPECT_EQ(motion.get(), Motor::Result::Stopped);
        hardware.drivers[1]->fault(); // Must not invoke a destroyed controller/motor.
        for (auto& driver : hardware.drivers) {
            driver->enable.reset();
        }
        Controller replacement{ CONFIGURATION, hardware.factory() };
        EXPECT_FALSE(replacement.enabled());
    }

    TEST(MotionController, PartialConstructionReleasesMotorsAndFaultObservers)
    {
        Hardware hardware;
        auto blocker{ hal::core::create_motor_feedback(Id::Motor2, { 1.8_deg, 16 }) };
        ASSERT_NE(blocker, nullptr);
        EXPECT_THROW((Controller{ CONFIGURATION, hardware.factory() }), std::runtime_error);
        hardware.drivers[0]->fault(); // M1 was constructed before M2 failed.
        EXPECT_EQ(hardware.enableLevel(), Level::High);
        blocker.reset();
        for (auto& driver : hardware.drivers) {
            driver->enable.reset();
        }
        Controller replacement{ CONFIGURATION, hardware.factory() };
        replacement.initializeDrivers();
        EXPECT_TRUE(replacement.ready());
    }
}
