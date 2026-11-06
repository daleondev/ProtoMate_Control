#pragma once

#include "Executor.hpp"
#include "Ownership.hpp"
#include "StepperMotor.hpp"
#include "common/motion/Trajectory.hpp"
#include "hal/core/core.hpp"

#include <array>
#include <functional>
#include <memory>

namespace io::motion
{
    // Owns the three independent motors and their shared hardware lifecycle.
    // Public operations run in thread context; destruction requires exclusive access.
    class Controller final
    {
      public:
        using MotorId = hal::core::MotorId;
        struct MotorConfiguration
        {
            pnm::units::Angle reference_position;
            pnm::units::Angle full_step_angle;
            std::size_t microsteps{};
        };
        using Configuration = std::array<MotorConfiguration, 3>;
        using Drivers = std::array<std::shared_ptr<hal::device::IStepperDriver>, 3>;
        // Optional dependency injection for tests. Production uses hal::core.
        using DriverFactory = std::function<Drivers(const std::array<std::size_t, 3>&,
                                                    const std::shared_ptr<hal::IDigitalOutput>&)>;

        struct Hardware
        {
            std::shared_ptr<hal::IDigitalOutput> enable;
            std::shared_ptr<hal::IStepGenerator> generator;
            std::array<std::shared_ptr<hal::IDigitalInput>, 3> references{};
        };
        // Omitted hardware uses the core catalog; test composition can substitute
        // only external devices while retaining the real timer/DMA implementation.
        // Creates motors/feedback and a diagnostic worker, but neither configures
        // drivers nor starts the STEP timebase nor energizes the motors.
        // The optional sink is shared by all motor workers.
        explicit Controller(const Configuration& configuration,
                            const DriverFactory& driver_factory = {},
                            std::shared_ptr<pnm::log::ISink> log_sink = {},
                            std::pmr::memory_resource* profile_memory = std::pmr::get_default_resource(),
                            const Hardware* hardware = nullptr);
        ~Controller();
        Controller(const Controller&) = delete;
        Controller& operator=(const Controller&) = delete;
        Controller(Controller&&) = delete;
        Controller& operator=(Controller&&) = delete;

        // Explicit recovery while disabled: cancel old work, invalidate references,
        // initialize all drivers and start the common timebase (without pulses).
        // Failure leaves the controller disabled and rejects motion until recovery.
        void initializeDrivers(std::stop_token stop = {});
        void enable(std::stop_token stop = {}); // Verification/settling is cancellable.
        void abort(std::optional<MotorId> motor = std::nullopt);
        void connectionLost();
        void disable(); // Stops/joins all work and invalidates references.
        void stop(std::optional<MotorId> motor = std::nullopt); // Retains holding torque.
        [[nodiscard]] bool enabled() const noexcept;
        [[nodiscard]] bool ready() const;
        [[nodiscard]] ::motion::trajectory::Reply trajectorySnapshot() const;
        ::motion::trajectory::Reply control(const ::motion::trajectory::Request& request);
        ::motion::trajectory::Reply prepareProgram(const ::motion::trajectory::Request& request,
                                                   std::stop_token stop = {});
        ::motion::trajectory::Error startProgram(const ::motion::trajectory::Run& run,
                                                 std::function<void(::motion::trajectory::Update)> completed);
        void releaseJob(std::uint64_t job);
        Ownership::Admission reserveMotor(std::uint64_t job, MotorId motor);

        // Inspection only: command submission goes through the controller so it
        // can serialize admission against disable, recovery and shared faults.
        [[nodiscard]] const StepperMotor& motor(MotorId id) const;
        pnm::Result<> setMotionDefaults(MotorId id, Defaults defaults);

        // Completion observers only notify a worker; they may run under command locks.
        std::future<StepperMotor::Result> moveRel(MotorId id,
                                                  pnm::units::Angle distance,
                                                  pnm::units::AngularVelocity velocity,
                                                  pnm::units::AngularAcceleration acceleration = 0_rad_s2,
                                                  pnm::units::AngularAcceleration deceleration = 0_rad_s2,
                                                  pnm::units::AngularJerk jerk = 0_rad_s3,
                                                  BufferMode buffer_mode = BufferMode::Aborting,
                                                  pnm::units::Time timeout = 0_s,
                                                  std::function<void()> completed = {});
        std::future<StepperMotor::Result> moveAbs(MotorId id,
                                                  pnm::units::Angle target,
                                                  pnm::units::AngularVelocity velocity,
                                                  pnm::units::AngularAcceleration acceleration = 0_rad_s2,
                                                  pnm::units::AngularAcceleration deceleration = 0_rad_s2,
                                                  pnm::units::AngularJerk jerk = 0_rad_s3,
                                                  BufferMode buffer_mode = BufferMode::Aborting,
                                                  pnm::units::Time timeout = 0_s,
                                                  std::function<void()> completed = {});
        std::future<StepperMotor::Result> reference(
          MotorId id,
          pnm::units::AngularVelocity seek_velocity = DEFAULT_REFERENCE_SEEK,
          pnm::units::AngularVelocity latch_velocity = DEFAULT_REFERENCE_LATCH,
          pnm::units::Time timeout = DEFAULT_REFERENCE_TIMEOUT,
          std::function<void()> completed = {});
        pnm::Result<hal::step::PulseCount> setVelocity(MotorId id, pnm::units::AngularVelocity velocity);

        // Small execution boundary for value-only, finite motor-space profiles.
        // The caller renews the command lease; neither API enables drivers.
        hal::util::Result<> execute(const Executor::Command& command);
        hal::util::Result<> prepareExecution(const Executor::Command& command);
        hal::util::Result<> startExecution(std::uint64_t id);
        bool renewExecution(std::uint64_t id);
        bool stopExecution(std::uint64_t id);
        Executor::Status executionStatus(std::uint64_t id = 0) const;

      private:
        struct Group;
        StepperMotor& axis(MotorId id) const;
        bool driversReady() const noexcept;
        bool canMoveLocked() const;
        bool velocityAllowed(MotorId motor, pnm::units::AngularVelocity velocity) const;
        void stopLocked(std::optional<MotorId> motor = std::nullopt);
        void invalidateReferencesLocked();
        void handleFaultsLocked();
        void monitorDrivers(const std::stop_token& stop);
        void executeMotions(std::stop_token stop);
        ::motion::trajectory::Reply trajectorySnapshotLocked() const;
        void initializeCapabilities();
        hal::util::Result<> prepareExecutionLocked(const Executor::Command& command, bool owned);
        void finishProgramsLocked();
        void serviceHomeLocked(Group& group);
        void stopProgramsLocked(std::uint8_t mask, bool abort);

        std::shared_ptr<hal::IDigitalOutput> m_enable;
        std::shared_ptr<hal::IStepGenerator> m_generator;
        // Callback destinations outlive every motor and its driver observer.
        runtime::Notification m_driverNotification;
        runtime::Notification m_controlNotification;
        runtime::Notification m_executionNotification;
        std::atomic_bool m_faulted{ false };
        // Serializes slow UART work, never acquired by stop/disable/admission.
        std::mutex m_operationMutex;
        mutable std::mutex m_mutex;
        std::uint64_t m_controlEpoch{};
        bool m_enabling{};
        Configuration m_configuration;
        ::motion::trajectory::Capabilities m_capabilities;
        std::pmr::memory_resource* m_profileMemory;
        bool m_initialized{}, m_admissionLost{};
        bool m_faultHandled{};
        std::array<std::unique_ptr<StepperMotor>, 3> m_motors;
        struct Group
        {
            std::unique_ptr<Executor> executor;
            std::uint64_t job{};
            std::uint8_t mask{}, moving{};
            bool managed{}, zero{}, started{}, notified{};
            double duration{};
            ::motion::trajectory::Error error{};
            std::chrono::steady_clock::time_point timeout{ std::chrono::steady_clock::time_point::max() };
            std::function<void(::motion::trajectory::Update)> completed;
            bool homing{}, home_finished{};
            std::array<::motion::trajectory::HomeStage, 3> home{};
            std::array<std::uint8_t, 3> home_order{};
            unsigned home_stage{};
            std::future<StepperMotor::Result> home_result;
            std::atomic_bool home_ready{};
        };
        std::array<Group, 3> m_groups;
        Ownership m_ownership;
        std::uint64_t m_lastExecution{}, m_boot{ 1 };
        std::jthread m_executionWorker;
        std::jthread m_driverWorker;
    };
}
