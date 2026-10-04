#pragma once

#include "StepperMotor.hpp"

#include <array>
#include <deque>
#include <vector>

namespace control
{
    // Application-owned hardware and command boundary shared by the CLI and a
    // future Robot. Neither client creates peripherals or starts the timebase.
    // Methods are thread-safe, thread-context only; callbacks never enter here.
    class MotionController final
    {
      public:
        using MotorId = hal::board::MotorId;
        using MotionId = std::uint64_t;
        struct AxisConfig
        {
            pnm::units::Angle reference_position;
            pnm::units::Angle full_step_angle;
            std::size_t microsteps;
        };
        struct Move
        {
            pnm::units::Angle position;
            pnm::units::AngularVelocity velocity;
            pnm::units::AngularAcceleration acceleration{ 0_rad_s2 };
            pnm::units::AngularAcceleration deceleration{ 0_rad_s2 };
            pnm::units::AngularJerk jerk{ 0_rad_s3 };
            StepperMotor::BufferMode buffer{ StepperMotor::BufferMode::Aborting };
            pnm::units::Time timeout{ 0_s };
            bool absolute{};
        };
        struct Motion
        {
            MotionId id;
            MotorId motor;
            // Empty means outstanding (planning, executing, or queued).
            std::optional<StepperMotor::Result> result;
            // Retained by the submitter even after the bounded results expire.
            // A Robot can await completion without polling this controller.
            std::shared_future<StepperMotor::Result> completion;
        };
        struct AxisStatus
        {
            MotorId motor;
            pnm::units::Angle position;
            pnm::units::AngularVelocity velocity;
            pnm::Result<pnm::units::Angle> actual_position;
            pnm::Result<pnm::units::AngularVelocity> actual_velocity;
            bool referenced;
            bool reference_switch_active;
            std::size_t outstanding;
        };
        struct Status
        {
            bool enabled;
            hal::step::Status generator;
            std::array<AxisStatus, 3> axes;
        };

        explicit MotionController(const std::array<AxisConfig, 3>& configuration);
        ~MotionController();
        MotionController(const MotionController&) = delete;
        MotionController& operator=(const MotionController&) = delete;

        // Enable includes the DM542T's 200 ms settling interval. Disabling
        // aborts all commands and invalidates references (holding torque lost).
        void enable();
        void disable();
        void stop(std::optional<MotorId> motor = std::nullopt);
        // Explicit fault recovery, only while disabled; does not enable motors.
        void reset();

        Motion move(MotorId motor, const Move& request);
        Motion reference(MotorId motor,
                         pnm::units::AngularVelocity seek = 5_rpm,
                         pnm::units::AngularVelocity latch = 0.5_rpm,
                         pnm::units::Time timeout = 30_s);
        hal::step::PulseCount setVelocity(MotorId motor, pnm::units::AngularVelocity velocity);
        StepperMotor::MotionDefaults defaults(MotorId motor) const;
        void setDefaults(MotorId motor, StepperMotor::MotionDefaults defaults);
        Status status();
        // Bounded to the most recent 32 commands, retaining outstanding ones.
        std::vector<Motion> motions();

      private:
        StepperMotor& axis(MotorId motor) const;
        void requireEnabled() const;
        void stopLocked(std::optional<MotorId> motor);
        void collect();
        void reserveMotion(MotorId motor);
        Motion finishSubmission(std::future<StepperMotor::Result> future);

        mutable std::mutex m_mutex;
        std::shared_ptr<hal::IDigitalOutput> m_enable;
        std::shared_ptr<hal::IStepGenerator> m_generator;
        std::array<std::unique_ptr<StepperMotor>, 3> m_motors;
        std::deque<Motion> m_motions;
        MotionId m_nextId{ 1 };
    };
}
