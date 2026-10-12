#pragma once

#include "StepperMotor.hpp"
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
            std::size_t microsteps;
        };
        using Configuration = std::array<MotorConfiguration, 3>;
        using Drivers = std::array<std::shared_ptr<hal::device::IStepperDriver>, 3>;
        // Optional dependency injection for tests. Production uses hal::core.
        using DriverFactory = std::function<Drivers(const std::array<std::size_t, 3>&,
                                                   const std::shared_ptr<hal::IDigitalOutput>&)>;

        // Creates motors/feedback and a diagnostic worker, but neither configures
        // drivers nor starts the STEP timebase nor energizes the motors.
        explicit Controller(const Configuration& configuration, const DriverFactory& driver_factory = {});
        ~Controller();
        Controller(const Controller&) = delete;
        Controller& operator=(const Controller&) = delete;
        Controller(Controller&&) = delete;
        Controller& operator=(Controller&&) = delete;

        // Explicit recovery while disabled: cancel old work, invalidate references,
        // initialize all drivers and start the common timebase (without pulses).
        // Failure leaves the controller disabled and rejects motion until recovery.
        void initializeDrivers();
        void enable(); // Verifies every driver; waits for shared enable to settle.
        void disable(); // Stops/joins all work and invalidates references.
        void stop(std::optional<MotorId> motor = std::nullopt); // Retains holding torque.
        [[nodiscard]] bool enabled() const noexcept;
        [[nodiscard]] bool ready() const;

        // Inspection only: command submission goes through the controller so it
        // can serialize admission against disable, recovery and shared faults.
        [[nodiscard]] const StepperMotor& motor(MotorId id) const;
        pnm::Result<> setMotionDefaults(MotorId id, Defaults defaults);

        std::future<StepperMotor::Result> moveRel(
          MotorId id, pnm::units::Angle distance, pnm::units::AngularVelocity velocity,
          pnm::units::AngularAcceleration acceleration = 0_rad_s2,
          pnm::units::AngularAcceleration deceleration = 0_rad_s2,
          pnm::units::AngularJerk jerk = 0_rad_s3, BufferMode buffer_mode = BufferMode::Aborting,
          pnm::units::Time timeout = 0_s);
        std::future<StepperMotor::Result> moveAbs(
          MotorId id, pnm::units::Angle target, pnm::units::AngularVelocity velocity,
          pnm::units::AngularAcceleration acceleration = 0_rad_s2,
          pnm::units::AngularAcceleration deceleration = 0_rad_s2,
          pnm::units::AngularJerk jerk = 0_rad_s3, BufferMode buffer_mode = BufferMode::Aborting,
          pnm::units::Time timeout = 0_s);
        std::future<StepperMotor::Result> reference(
          MotorId id, pnm::units::AngularVelocity seek_velocity = DEFAULT_REFERENCE_SEEK,
          pnm::units::AngularVelocity latch_velocity = DEFAULT_REFERENCE_LATCH,
          pnm::units::Time timeout = DEFAULT_REFERENCE_TIMEOUT);
        pnm::Result<hal::step::PulseCount> setVelocity(MotorId id, pnm::units::AngularVelocity velocity);

      private:
        StepperMotor& axis(MotorId id) const;
        bool driversReady() const noexcept;
        bool canMoveLocked() const;
        void stopLocked(std::optional<MotorId> motor = std::nullopt);
        void invalidateReferencesLocked();
        void handleFaultsLocked();
        void monitorDrivers(const std::stop_token& stop);

        std::shared_ptr<hal::IDigitalOutput> m_enable;
        std::shared_ptr<hal::IStepGenerator> m_generator;
        // Callback destinations outlive every motor and its driver observer.
        runtime::Notification m_driverNotification;
        std::atomic_bool m_faulted{ false };
        mutable std::mutex m_mutex;
        bool m_initialized{};
        bool m_faultHandled{};
        std::array<std::unique_ptr<StepperMotor>, 3> m_motors;
        std::jthread m_driverWorker;
    };
}
