#pragma once

#include "AxisConversion.hpp"
#include "StepperMotor.hpp"
#include "hal/devices/impl/Tmc2209.hpp"

#include <array>
#include <deque>
#include <functional>
#include <variant>
#include <vector>

namespace control
{
    // Application-owned hardware and command boundary shared by the CLI and a
    // Robot. Neither client creates peripherals or starts the timebase.
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
        template<typename Position, typename Speed, typename Acceleration, typename Jerk>
        struct MoveRequest
        {
            Position position;
            Speed velocity;
            Acceleration acceleration{};
            Acceleration deceleration{};
            Jerk jerk{};
            StepperMotor::BufferMode buffer{ StepperMotor::BufferMode::Aborting };
            pnm::units::Time timeout{ 0_s };
            bool absolute{};
        };
        using Move = MoveRequest<pnm::units::Angle,
                                 pnm::units::AngularVelocity,
                                 pnm::units::AngularAcceleration,
                                 pnm::units::AngularJerk>;
        using LinearMove =
          MoveRequest<pnm::units::Distance, pnm::units::Velocity, pnm::units::Acceleration, pnm::units::Jerk>;
        using Conversion = std::variant<RotaryAxisConversion, LinearAxisConversion>;
        using AxisMove = std::variant<Move, LinearMove>;
        using AxisSpeed = std::variant<pnm::units::AngularVelocity, pnm::units::Velocity>;
        struct LinearDefaults
        {
            pnm::units::Acceleration acceleration;
            pnm::units::Acceleration deceleration;
            pnm::units::Jerk jerk;
        };
        using AxisDefaults = std::variant<StepperMotor::MotionDefaults, LinearDefaults>;
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
            // Configuration and motor state from the same locked snapshot.
            // Empty until the application's mechanics have been supplied.
            std::optional<Conversion> conversion;
            StepperMotor::FeedbackSource feedback_source;
            pnm::units::Angle feedback_resolution;
        };
        struct Status
        {
            bool enabled;
            hal::step::Status generator;
            std::array<AxisStatus, 3> axes;
            bool coordinated{};
        };

        using DriverConfiguration = hal::device::Tmc2209::Configuration;
        struct DriverStatus
        {
            MotorId motor;
            std::uint8_t address;
            DriverConfiguration configuration;
            std::optional<hal::device::Tmc2209::Status> diagnostics;
            std::string error;
            bool ready;
            bool fault_latched;
        };
        std::array<DriverStatus, 2> driverStatus() const;
        struct AlarmStatus
        {
            bool active; // DM542T ALM is open (fault or disconnected cable).
            bool fault_latched;
        };
        AlarmStatus alarmStatus() const;
        // Explicit recovery, disabled only. Never automatically resumes motion.
        void initializeDrivers();
        void configureDriver(MotorId motor, DriverConfiguration configuration);

        struct CoordinatedPlan
        {
            // Empty sequence means this axis holds its position.
            std::array<std::shared_ptr<const hal::step::Sequence>, 3> sequences;
            std::array<std::optional<std::chrono::nanoseconds>, 3> delays;
            std::array<bool, 3> forward{};
            pnm::units::Time duration{};
        };
        enum class Operation
        {
            Move,
            Reference
        };
        struct GroupMotion
        {
            MotionId id;
            Operation operation;
            pnm::units::Time duration;
            std::optional<StepperMotor::Result> result;
            std::shared_future<StepperMotor::Result> completion;
        };
        using Planner =
          std::function<CoordinatedPlan(const Status&, const std::array<StepperMotor::MotionDefaults, 3>&)>;

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

        // Require idle axes and reserve all three through completion/cancellation.
        // Planner runs synchronously under the submission lock: its snapshot
        // cannot go stale before the hardware is prepared. It must not call back
        // into this controller. A zero-pulse plan completes without arming.
        GroupMotion coordinate(const Planner& planner, pnm::units::Time timeout = 0_s);
        GroupMotion referenceAll(pnm::units::Time timeout = 90_s);
        std::vector<GroupMotion> groupMotions();

        AxisConfig motorConfiguration(MotorId motor) const;
        // M1 = shoulder, M2 = relative elbow, M3 = Z. Reconfiguration requires
        // disabled drivers. Reference coordinates must agree with the motor.
        void configureAxis(MotorId motor, Conversion conversion);
        Conversion axisConversion(MotorId motor) const;
        // Strong axis units are converted under the same lock as submission,
        // so configuration cannot change between conversion and execution.
        Motion moveAxis(MotorId motor, const AxisMove& request);
        Motion referenceAxis(MotorId motor,
                             std::optional<AxisSpeed> seek = {},
                             std::optional<AxisSpeed> latch = {},
                             pnm::units::Time timeout = 30_s);
        hal::step::PulseCount setAxisVelocity(MotorId motor, const AxisSpeed& velocity);
        AxisDefaults axisDefaults(MotorId motor) const;
        void setAxisDefaults(MotorId motor, const AxisDefaults& defaults);

      private:
        StepperMotor& axis(MotorId motor) const;
        void requireEnabled() const;
        void initializeDriversLocked();
        void monitorDrivers(std::stop_token stop);
        void driverFaultLocked(std::size_t index, std::string reason);
        void alarmFaultLocked();
        void stopLocked(std::optional<MotorId> motor);
        void collect();
        void reserveMotion(MotorId motor);
        Motion finishSubmission(std::future<StepperMotor::Result> future);
        Motion moveLocked(MotorId motor, const Move& request);
        Motion referenceLocked(MotorId motor,
                               pnm::units::AngularVelocity seek,
                               pnm::units::AngularVelocity latch,
                               pnm::units::Time timeout);
        hal::step::PulseCount setVelocityLocked(MotorId motor, pnm::units::AngularVelocity velocity);
        const Conversion& conversionLocked(MotorId motor) const;
        Status statusLocked();
        void requireManualAccess() const;
        void requireIdleGroup();
        GroupMotion launchGroup(Operation operation, CoordinatedPlan plan, pnm::units::Time timeout);
        void runGroup(Operation operation,
                      CoordinatedPlan plan,
                      pnm::units::Time timeout,
                      std::shared_ptr<std::promise<StepperMotor::Result>> completion,
                      std::stop_token stop) noexcept;

        mutable std::mutex m_mutex;
        const std::array<AxisConfig, 3> m_configuration;
        std::array<std::optional<Conversion>, 3> m_conversions;
        runtime::Notification m_groupNotification;
        std::mutex m_groupActionMutex;
        std::atomic_bool m_groupActive{};
        std::shared_ptr<hal::IDigitalOutput> m_enable;
        std::shared_ptr<hal::IStepGenerator> m_generator;
        std::array<std::unique_ptr<StepperMotor>, 3> m_motors;
        std::deque<Motion> m_motions;
        MotionId m_nextId{ 1 };
        std::deque<GroupMotion> m_groupMotions;
        MotionId m_nextGroupId{ 1 };
        std::jthread m_groupWorker;
        std::shared_ptr<hal::IUart> m_driverBus;
        std::array<std::unique_ptr<hal::device::Tmc2209>, 2> m_drivers;
        std::array<std::shared_ptr<hal::IDigitalInput>, 2> m_diagnostics;
        std::shared_ptr<hal::IDigitalInput> m_alarm;
        static constexpr unsigned AlarmFault = 1U << 2;
        bool m_alarmHandled{};
        std::array<DriverConfiguration, 2> m_driverConfigurations{{
            { .run_milliamps = 650, .hold_milliamps = 650, .mode = hal::device::Tmc2209::Mode::StealthChop },
            { .run_milliamps = 550, .hold_milliamps = 550, .mode = hal::device::Tmc2209::Mode::StealthChop },
        }};
        std::array<std::optional<hal::device::Tmc2209::Status>, 2> m_driverStatus;
        std::array<std::string, 2> m_driverErrors;
        bool m_driversReady{};
        std::atomic_uint m_driverFaults{};
        runtime::Notification m_driverNotification;
        std::jthread m_driverWorker;
    };
}
