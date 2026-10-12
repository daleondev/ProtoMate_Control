#pragma once

#include "ControllerTypes.hpp"
#include "StepperMotor.hpp"
#include "hal/devices/itf/IConfigurableStepperDriver.hpp"

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
    class MotionController final : public ControllerTypes
    {
      public:
        std::array<DriverStatus, 3> driverStatus() const;
        // Explicit recovery, disabled only. Never automatically resumes motion.
        void initializeDrivers();
        void configureDriver(MotorId id, DriverConfiguration configuration);

        struct CoordinatedPlan
        {
            // Empty sequence means this axis holds its position.
            std::array<std::shared_ptr<const hal::step::Sequence>, 3> sequences;
            std::array<std::optional<std::chrono::nanoseconds>, 3> delays;
            std::array<bool, 3> forward{};
            pnm::units::Time duration;
        };
        using Planner = std::function<CoordinatedPlan(const Status&, const std::array<MotionDefaults, 3>&)>;
        PlanningSnapshot planningSnapshot();
        GroupMotion coordinate(const Trajectory& /*trajectory*/, pnm::units::Time timeout = 0_s);
        // Nonblocking hardware inhibition, followed by stop() on the IO worker.
        void inhibitRemoteMotion() noexcept;
        void requestStop(std::optional<MotorId> motor = std::nullopt) noexcept;
        void cancelMotion(MotionId id, bool group);
        void recoverRemoteMotion();

        using DriverFactory = std::function<std::array<std::shared_ptr<hal::device::IStepperDriver>, 3>(
          const std::array<std::size_t, 3>&,
          const std::shared_ptr<hal::IDigitalOutput>&)>;
        // Empty factory selects the board's drivers. Alternate compositions can
        // supply the same interfaces without exposing transports to this class.
        explicit MotionController(const std::array<AxisConfig, 3>& configuration,
                                  const DriverFactory& driver_factory = {});
        ~MotionController();
        MotionController(const MotionController&) = delete;
        MotionController& operator=(const MotionController&) = delete;
        MotionController(MotionController&&) = delete;
        MotionController& operator=(MotionController&&) = delete;

        // Enable includes the DM542T's 200 ms settling interval. Disabling
        // aborts all commands and invalidates references (holding torque lost).
        void enable();
        void disable();
        void stop(std::optional<MotorId> motor = std::nullopt);
        // Explicit fault recovery, only while disabled; does not enable motors.
        void reset();

        Motion move(MotorId motor, const Move& request);
        Motion reference(MotorId motor,
                         pnm::units::AngularVelocity seek = control::DEFAULT_REFERENCE_SEEK,
                         pnm::units::AngularVelocity latch = control::DEFAULT_REFERENCE_LATCH,
                         pnm::units::Time timeout = control::DEFAULT_REFERENCE_TIMEOUT);
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
        GroupMotion referenceAll(pnm::units::Time timeout = control::DEFAULT_GROUP_REFERENCE_TIMEOUT);
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
                             pnm::units::Time timeout = control::DEFAULT_REFERENCE_TIMEOUT);
        hal::step::PulseCount setAxisVelocity(MotorId motor, const AxisSpeed& velocity);
        AxisDefaults axisDefaults(MotorId motor) const;
        void setAxisDefaults(MotorId motor, const AxisDefaults& defaults);

      private:
        CoordinatedPlan planTrajectory(const Trajectory& trajectory,
                                       const Status& state,
                                       const std::array<MotionDefaults, 3>& defaults);
        MotorResult referenceGroup(std::chrono::steady_clock::time_point deadline,
                                   const std::stop_token& stop);
        MotorResult waitForReference(std::size_t index,
                                     std::future<MotorResult>& homing,
                                     const std::array<bool, 3>& referenced,
                                     std::chrono::steady_clock::time_point deadline,
                                     const std::stop_token& stop);
        MotorResult moveGroup(const CoordinatedPlan& plan,
                              std::chrono::steady_clock::time_point deadline,
                              const std::stop_token& stop);
        MotorResult waitForGroup(const CoordinatedPlan& plan,
                                 std::chrono::steady_clock::time_point deadline,
                                 const std::stop_token& stop);

        StepperMotor& axis(MotorId motor) const;
        void requireEnabled() const;
        void initializeDriversLocked();
        void monitorDrivers(const std::stop_token& stop);
        bool driversReady() const noexcept;
        void handleDriverFaultsLocked();
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
                      const CoordinatedPlan& plan,
                      pnm::units::Time timeout,
                      Completion<MotorResult> completion,
                      const std::stop_token& stop) noexcept;

        mutable std::mutex m_mutex;
        std::atomic_bool m_remoteInhibited;
        std::uint64_t m_configurationRevision{ 1 }, m_commandRevision{ 1 };
        std::array<AxisConfig, 3> m_configuration;
        std::array<std::optional<Conversion>, 3> m_conversions;
        runtime::Notification m_groupNotification;
        std::mutex m_groupActionMutex;
        std::atomic_bool m_groupActive;
        std::shared_ptr<hal::IDigitalOutput> m_enable;
        std::shared_ptr<hal::IStepGenerator> m_generator;
        std::array<std::unique_ptr<StepperMotor>, 3> m_motors;
        std::deque<Motion> m_motions;
        MotionId m_nextId{ 1 };
        std::deque<GroupMotion> m_groupMotions;
        MotionId m_nextGroupId{ 1 };
        std::jthread m_groupWorker;
        // Aggregate shutdown events only; each motor's driver owns its state.
        std::atomic_uint m_driverFaults;
        unsigned m_handledDriverFaults{};
        runtime::Notification m_driverNotification;
        std::jthread m_driverWorker;
    };
}
