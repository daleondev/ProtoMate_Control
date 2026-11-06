#include "hal/drivers/impl/linux/Gpio.hpp"
#include "io/motion/Controller.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <type_traits>
#include <semaphore>

namespace
{
    using namespace pnm::units::literals;
    using namespace std::chrono_literals;
    using Controller = io::motion::Controller;
    using Motor = io::motion::StepperMotor;
    using Id = Controller::MotorId;
    using Level = hal::gpio::Level;
    constexpr Controller::Configuration CONFIGURATION{ Controller::MotorConfiguration{ 135_deg, 1.8_deg, 16 },
                                                       Controller::MotorConfiguration{ 135_deg, 1.8_deg, 16 },
                                                       Controller::MotorConfiguration{ 0_deg, 1.8_deg, 16 } };

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
            m_status = { .fault_latched = true,
                         .fault = Fault::Electrical,
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
        std::array<std::size_t, 3> expected_microsteps{16, 16, 16};
        std::array<std::shared_ptr<Driver>, 3> drivers{ std::make_shared<Driver>(),
                                                        std::make_shared<Driver>(),
                                                        std::make_shared<Driver>() };
        Controller::DriverFactory factory()
        {
            return [this](const std::array<std::size_t, 3>& microsteps,
                          const std::shared_ptr<hal::IDigitalOutput>& enable) {
                EXPECT_EQ(microsteps, expected_microsteps);
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
        EXPECT_THROW(controller.abort(static_cast<Id>(255)), std::invalid_argument);
        EXPECT_EQ(controller.reserveMotor(1, static_cast<Id>(255)).error,
                  motion::trajectory::Error::InvalidInput);
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
        EXPECT_NEAR(
          controller.motor(Id::Motor3).setpointPosition().get<pnm::units::AngleUnits::deg>(), -1.125, 0.0001);
    }

    TEST(MotionController, FaultDisablesAllMotorsCancelsPendingWorkAndDoesNotAutoRecover)
    {
        Hardware hardware;
        Controller controller{ CONFIGURATION, hardware.factory() };
        releaseSwitches();
        controller.initializeDrivers();
        controller.enable();
        auto active{ controller.moveRel(Id::Motor1, -90_deg, 5_rpm) };
        auto pending{ controller.moveRel(
          Id::Motor1, -90_deg, 5_rpm, 0_rad_s2, 0_rad_s2, 0_rad_s3, io::motion::BufferMode::Buffered) };
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

TEST(MotionController, PreparedExecutorOwnsAxesAndTracksPositionThroughCompletionAndFault)
{
    Hardware hardware;
    Controller controller{ CONFIGURATION, hardware.factory() };
    releaseSwitches();
    controller.initializeDrivers();
    controller.enable();
    io::motion::Executor::Command command{ .id = 1 };
    command.axes[0].limits = { 1000, 10000, 10000, 100000 };
    command.axes[0].profile = motion::Profile::create(100, command.axes[0].limits)->description();
    EXPECT_FALSE(controller.prepareExecution(command)); // Homing remains mandatory.
    auto input = hal::gpio::simulated_input({ hal::gpio::Port::E, 7 });
    ASSERT_NE(input, nullptr);
    auto homing = controller.reference(Id::Motor1, 10_rpm, 2_rpm, 2_s);
    ASSERT_TRUE(waitUntil([&] { return controller.motor(Id::Motor1).setpointVelocity() > 0_rpm; }));
    input->setSimulatedLevel(Level::High);
    ASSERT_TRUE(waitUntil([&] { return controller.motor(Id::Motor1).setpointVelocity() < 0_rpm; }));
    input->setSimulatedLevel(Level::Low);
    ASSERT_TRUE(waitUntil([&] { return controller.motor(Id::Motor1).setpointVelocity() > 0_rpm; }));
    input->setSimulatedLevel(Level::High);
    ASSERT_EQ(homing.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(homing.get(), Motor::Result::Completed);
    auto tooFast = command;
    tooFast.axes[0].limits.velocity = io::motion::Executor::MAX_PRODUCTION_RATE + 1.;
    const auto rejected = controller.prepareExecution(tooFast);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error(), std::errc::invalid_argument);
    EXPECT_EQ(controller.executionStatus().state, io::motion::Executor::State::Idle);
    ASSERT_TRUE(controller.prepareExecution(command)); // Backward away from switch.
    EXPECT_EQ(controller.moveRel(Id::Motor2, -1_deg, 1_rpm).get(), Motor::Result::Rejected);
    ASSERT_TRUE(controller.startExecution(1));
    EXPECT_FALSE(controller.startExecution(1));
    ASSERT_TRUE(waitUntil([&] {
        controller.renewExecution(1);
        const auto s = controller.executionStatus().state;
        return s == io::motion::Executor::State::Completed || s == io::motion::Executor::State::Stopped ||
               s == io::motion::Executor::State::Faulted;
    }));
    const auto status = controller.executionStatus();
    EXPECT_EQ(status.state, io::motion::Executor::State::Completed);
    EXPECT_EQ(status.pulses[0], 100U);
    EXPECT_NEAR(
      controller.motor(Id::Motor1).setpointPosition().get<pnm::units::AngleUnits::deg>(), 123.75, 1e-8);
    EXPECT_EQ(controller.motor(Id::Motor1).setpointVelocity(), 0_rpm);
    EXPECT_TRUE(controller.enabled());
    command.id = 2;
    ASSERT_TRUE(controller.prepareExecution(command));
    ASSERT_TRUE(controller.startExecution(2));
    hardware.drivers[0]->fault();
    EXPECT_EQ(hardware.enableLevel(), Level::High);
    ASSERT_TRUE(
      waitUntil([&] { return controller.executionStatus().state == io::motion::Executor::State::Faulted; }));
    EXPECT_FALSE(controller.renewExecution(2));
    command.id = 3;
    EXPECT_FALSE(controller.prepareExecution(command));
}

TEST(MotionController, FallbackResolutionReachesDriverFeedbackAndFingerprintTogether)
{
    std::uint64_t previous{};
    {
        Hardware hardware;
        Controller controller{CONFIGURATION, hardware.factory()};
        previous = controller.trajectorySnapshot().capabilities.hardware_fingerprint;
    }
    auto config = CONFIGURATION;
    config[2].microsteps = 8;
    Hardware hardware;
    hardware.expected_microsteps[2] = 8;
    Controller controller{config, hardware.factory()};
    const auto snapshot = controller.trajectorySnapshot();
    EXPECT_DOUBLE_EQ(snapshot.capabilities.step_radians[2], 2 * snapshot.capabilities.step_radians[1]);
    EXPECT_NE(snapshot.capabilities.hardware_fingerprint, previous);
    EXPECT_EQ(snapshot.referenced, 0U);
    EXPECT_EQ(snapshot.capabilities.limits[2].velocity, io::motion::Executor::MAX_PRODUCTION_RATE);
    EXPECT_EQ(controller.motor(Id::Motor2).feedbackResolution(),
              controller.motor(Id::Motor3).feedbackResolution()); // INDEX remains four full steps.
}

TEST(MotionController, ProductionRateLimitAppliesToIndependentCommandsToo)
{
    Hardware hardware;
    Controller controller{CONFIGURATION, hardware.factory()};
    releaseSwitches();
    controller.initializeDrivers();
    controller.enable();
    const auto tooFast = 1_rad_s *
      ((io::motion::Executor::MAX_PRODUCTION_RATE + 1) * CONFIGURATION[0].full_step_angle.get() / 16);
    EXPECT_EQ(controller.moveRel(Id::Motor1, -1_deg, tooFast).get(), Motor::Result::Rejected);
    EXPECT_EQ(controller.moveAbs(Id::Motor1, 1_deg, tooFast).get(), Motor::Result::Rejected);
    EXPECT_EQ(controller.reference(Id::Motor1, tooFast).get(), Motor::Result::Rejected);
    EXPECT_FALSE(controller.setVelocity(Id::Motor1, tooFast));
    EXPECT_EQ(controller.motor(Id::Motor1).setpointVelocity(), 0_rpm);
}

TEST(MotionController, ReservationsExcludeFamiliesAndRetainIndependentAxisAdmission)
{
    namespace tr=motion::trajectory;
    Hardware hw; Controller controller{CONFIGURATION,hw.factory()};
    releaseSwitches(); controller.initializeDrivers(); controller.enable();
    tr::Request r{.operation=tr::Operation::Acquire,.boot=controller.trajectorySnapshot().boot,.configuration=1};
    r.family=motion::Family::Axis;r.axes=1;r.job=10;
    const auto first=controller.control(r);ASSERT_EQ(first.error,tr::Error::None);
    const auto raw=controller.reserveMotor(11,Id::Motor2);
    EXPECT_EQ(raw.error,tr::Error::Busy);EXPECT_EQ(raw.busy_job,10U);
    r.family=motion::Family::Robot;r.axes=7;r.job=12;
    const auto robot=controller.control(r);EXPECT_EQ(robot.error,tr::Error::Busy);EXPECT_EQ(robot.job,10U);
    r.family=motion::Family::Axis;r.axes=2;r.job=13;
    EXPECT_EQ(controller.control(r).error,tr::Error::None);
    r.job=14;EXPECT_EQ(controller.control(r).error,tr::Error::Busy);
    r.operation=tr::Operation::Stop;r.axes=1;r.job=10;
    EXPECT_EQ(controller.control(r).error,tr::Error::None);
    const auto snapshot=controller.trajectorySnapshot();
    EXPECT_EQ(snapshot.owners[0],0U);EXPECT_EQ(snapshot.owners[1],13U);
    r.operation=tr::Operation::Prepare;r.session=first.session;
    EXPECT_EQ(controller.prepareProgram(r).error,tr::Error::Stale);
    controller.releaseJob(13);
    EXPECT_EQ(controller.trajectorySnapshot().family,motion::Family::None);
}

TEST(MotionController, ExpiredPreparationCannotBeRevivedAndReconnectRequiresRecovery)
{
    namespace tr=motion::trajectory;
    Hardware hw; Controller controller{CONFIGURATION,hw.factory()};
    releaseSwitches(); controller.initializeDrivers(); controller.enable();
    tr::Request r{.operation=tr::Operation::Acquire,.boot=controller.trajectorySnapshot().boot,.configuration=1};
    r.family=motion::Family::Robot;r.axes=7;r.job=20;
    const auto acquired=controller.control(r);ASSERT_EQ(acquired.error,tr::Error::None);
    ASSERT_TRUE(waitUntil([&]{return controller.trajectorySnapshot().family==motion::Family::None;}));
    r.operation=tr::Operation::Renew;r.renew_jobs[0]=20;controller.control(r);
    r.operation=tr::Operation::Prepare;r.session=acquired.session;
    EXPECT_EQ(controller.prepareProgram(r).error,tr::Error::Stale);
    controller.connectionLost();
    EXPECT_TRUE(controller.enabled());EXPECT_FALSE(controller.ready());
    r.operation=tr::Operation::Acquire;r.job=21;
    EXPECT_EQ(controller.control(r).error,tr::Error::Fault);
    controller.disable();controller.initializeDrivers();controller.enable();
    EXPECT_EQ(controller.control(r).error,tr::Error::None);
    controller.releaseJob(21);
}

TEST(MotionController, RawBufferedOwnershipSurvivesReleaseOfItsPredecessor)
{
    namespace tr=motion::trajectory;
    Hardware hw; Controller controller{CONFIGURATION,hw.factory()};
    releaseSwitches(); controller.initializeDrivers();controller.enable();
    ASSERT_EQ(controller.reserveMotor(1,Id::Motor1).error,tr::Error::None);
    ASSERT_EQ(controller.reserveMotor(2,Id::Motor1).error,tr::Error::None);
    controller.releaseJob(1);
    tr::Request r{.operation=tr::Operation::Acquire,.boot=controller.trajectorySnapshot().boot,.configuration=1};
    r.family=motion::Family::Axis;r.axes=2;r.job=3;
    const auto busy=controller.control(r);EXPECT_EQ(busy.error,tr::Error::Busy);EXPECT_EQ(busy.job,2U);
    controller.releaseJob(2);
    EXPECT_EQ(controller.control(r).error,tr::Error::None);controller.releaseJob(3);
}

TEST(MotionController, AbortingOneMotorDoesNotReuseAReleasedRobotGroupMask)
{
    namespace tr = motion::trajectory;
    Hardware hardware;
    Controller controller{ CONFIGURATION, hardware.factory() };
    releaseSwitches();
    controller.initializeDrivers();
    controller.enable();

    tr::Request request{ .operation = tr::Operation::Acquire,
                         .boot = controller.trajectorySnapshot().boot,
                         .configuration = 1 };
    request.family = motion::Family::Robot;
    request.axes = 7;
    request.job = 50;
    const auto reservation = controller.control(request);
    ASSERT_EQ(reservation.error, tr::Error::None);
    request.operation = tr::Operation::Prepare;
    request.session = reservation.session;
    request.expected_position = reservation.position;
    request.axis_revisions = reservation.axis_revisions;
    request.program.kind = tr::ProgramKind::Home;
    request.program.home_order = { 0, 1, 2 };
    for (auto& home : request.program.home) home = { 5, 0.5, 30 };
    ASSERT_EQ(controller.prepareProgram(request).error, tr::Error::None);
    request.operation = tr::Operation::Stop;
    ASSERT_EQ(controller.control(request).error, tr::Error::None);
    ASSERT_EQ(controller.trajectorySnapshot().family, motion::Family::None);

    auto first = controller.moveRel(Id::Motor1, -90_deg, 5_rpm);
    auto second = controller.moveRel(Id::Motor2, -90_deg, 5_rpm);
    ASSERT_TRUE(waitUntil([&] {
        return controller.motor(Id::Motor1).setpointVelocity() < 0_rpm &&
               controller.motor(Id::Motor2).setpointVelocity() < 0_rpm;
    }));
    controller.abort(Id::Motor1);
    EXPECT_EQ(first.get(), Motor::Result::Stopped);
    EXPECT_EQ(second.wait_for(20ms), std::future_status::timeout);
    EXPECT_LT(controller.motor(Id::Motor2).setpointVelocity(), 0_rpm);
    controller.abort();
    EXPECT_EQ(second.get(), Motor::Result::Stopped);
}

TEST(MotionController, CompilationLeavesAdmissionUnlockedAndCancellationCannotArm)
{
    namespace tr=motion::trajectory;
    struct PausedMemory final : std::pmr::memory_resource {
        std::binary_semaphore entered{0}, resume{0};
        std::atomic_bool pause{true};
        void* do_allocate(std::size_t bytes,std::size_t alignment) override {
            if(pause.exchange(false)){entered.release();resume.acquire();}
            return std::pmr::new_delete_resource()->allocate(bytes,alignment);
        }
        void do_deallocate(void* p,std::size_t bytes,std::size_t alignment) override {
            std::pmr::new_delete_resource()->deallocate(p,bytes,alignment);
        }
        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override{return this==&other;}
    } memory;
    Hardware hw; Controller controller{CONFIGURATION,hw.factory(),{},&memory};
    releaseSwitches();controller.initializeDrivers();controller.enable();
    auto input=hal::gpio::simulated_input({hal::gpio::Port::E,7});
    auto homing=controller.reference(Id::Motor1,10_rpm,2_rpm,2_s);
    ASSERT_TRUE(waitUntil([&]{return controller.motor(Id::Motor1).setpointVelocity()>0_rpm;}));
    input->setSimulatedLevel(Level::High);
    ASSERT_TRUE(waitUntil([&]{return controller.motor(Id::Motor1).setpointVelocity()<0_rpm;}));
    input->setSimulatedLevel(Level::Low);
    ASSERT_TRUE(waitUntil([&]{return controller.motor(Id::Motor1).setpointVelocity()>0_rpm;}));
    input->setSimulatedLevel(Level::High);
    ASSERT_EQ(homing.get(),Motor::Result::Completed);
    tr::Request r{.operation=tr::Operation::Acquire,.boot=controller.trajectorySnapshot().boot,.configuration=1};
    r.family=motion::Family::Axis;r.axes=1;r.job=44;
    const auto acquired=controller.control(r);ASSERT_EQ(acquired.error,tr::Error::None);
    r.operation=tr::Operation::Prepare;r.session=acquired.session;
    r.expected_position=acquired.position;r.axis_revisions=acquired.axis_revisions;
    r.program.displacement[0]=-100;
    r.program.limits[0]=acquired.capabilities.limits[0];
    r.program.progress_limits={1,10,10,100};
    r.program.progress=motion::Profile::create(1,r.program.progress_limits)->description();
    tr::Reply result;
    auto compile=std::jthread([&]{result=controller.prepareProgram(r);});
    ASSERT_TRUE(memory.entered.try_acquire_for(1s));
    // If compilation still held Controller's mutex this call would deadlock.
    // The watchdog unblocks the allocator so regression failure stays bounded.
    std::atomic_bool released{};
    auto watchdog=std::jthread([&]{std::this_thread::sleep_for(500ms);if(!released.exchange(true))memory.resume.release();});
    auto cancel=r;cancel.operation=tr::Operation::Stop;
    const auto start=std::chrono::steady_clock::now();
    EXPECT_EQ(controller.control(cancel).error,tr::Error::None);
    EXPECT_LT(std::chrono::steady_clock::now()-start,250ms);
    if(!released.exchange(true))memory.resume.release();
    compile.join();
    EXPECT_EQ(result.error,tr::Error::Stale);
    EXPECT_EQ(controller.trajectorySnapshot().family,motion::Family::None);
    EXPECT_EQ(controller.motor(Id::Motor1).setpointVelocity(),0_rpm);
    EXPECT_EQ(controller.motor(Id::Motor1).setpointPosition(),135_deg);
}
