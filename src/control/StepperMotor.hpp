#pragma once

#include "pneumo/units.hpp"
#include "runtime/synchronization/Notification.hpp"

#include <atomic>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

#include "hal/board/board.hpp"
#include "hal/devices/itf/IMotorFeedback.hpp"

using namespace pnm::units::literals;

namespace control { class MotionController; }

class StepperMotor final
{
  public:
    enum class Direction
    {
        Forward,
        Backward
    };

    enum class Result
    {
        Completed,
        Stopped,
        TimedOut,
        Rejected,
        Faulted
    };

    enum class BufferMode
    {
        Aborting,
        Buffered,
        BlendingLow,
        BlendingPrevious,
        BlendingNext,
        BlendingHigh
    };

    struct MotionDefaults
    {
        pnm::units::AngularAcceleration acceleration{ 3600_deg_s2 };
        pnm::units::AngularAcceleration deceleration{ 3600_deg_s2 };
        // A configured zero disables the jerk limit (trapezoidal profile).
        pnm::units::AngularJerk jerk{ 36000_deg_s3 };
    };

    // Driver is required, initialized by the shared controller while disabled.
    // An omitted feedback provider is selected through the board factory.
    StepperMotor(hal::board::MotorId id,
                 pnm::units::Angle reference_switch_position,
                 pnm::units::Angle full_step_angle,
                 size_t microsteps,
                 const std::shared_ptr<hal::IStepGenerator>& step_generator,
                 std::shared_ptr<hal::device::IStepperDriver> driver,
                 std::shared_ptr<hal::device::IMotorFeedback> feedback = {},
                 hal::device::IStepperDriver::FaultCallback shutdown = {});
    hal::device::IStepperDriver& driver() const noexcept { return *m_driver; }
    ~StepperMotor();

    StepperMotor(const StepperMotor&) = delete;
    StepperMotor& operator=(const StepperMotor&) = delete;
    StepperMotor(StepperMotor&&) = delete;
    StepperMotor& operator=(StepperMotor&&) = delete;

    // Dynamics are positive magnitudes; zero selects the axis default.
    // One pending successor; each call owns an independent completion future.
    std::future<Result> moveRel(pnm::units::Angle distance,
                                pnm::units::AngularVelocity velocity,
                                pnm::units::AngularAcceleration acceleration = 0_rad_s2,
                                pnm::units::AngularAcceleration deceleration = 0_rad_s2,
                                pnm::units::AngularJerk jerk = 0_rad_s3,
                                BufferMode buffer_mode = BufferMode::Aborting,
                                pnm::units::Time timeout = 0_s);

    std::future<Result> moveAbs(pnm::units::Angle target,
                                pnm::units::AngularVelocity velocity,
                                pnm::units::AngularAcceleration acceleration = 0_rad_s2,
                                pnm::units::AngularAcceleration deceleration = 0_rad_s2,
                                pnm::units::AngularJerk jerk = 0_rad_s3,
                                BufferMode buffer_mode = BufferMode::Aborting,
                                pnm::units::Time timeout = 0_s);
    // Preserve the existing timeout-only call form.
    std::future<Result> moveRel(pnm::units::Angle distance,
                                pnm::units::AngularVelocity velocity,
                                pnm::units::Time timeout);
    std::future<Result> moveAbs(pnm::units::Angle target,
                                pnm::units::AngularVelocity velocity,
                                pnm::units::Time timeout);
    pnm::Result<> setMotionDefaults(MotionDefaults defaults);
    MotionDefaults motionDefaults() const;

    std::future<Result> reference(pnm::units::AngularVelocity seek_velocity = 5_rpm,
                                  pnm::units::AngularVelocity latch_velocity = 0.5_rpm,
                                  pnm::units::Time timeout = 30_s);
    bool isReferenced() const noexcept;
    bool referenceSwitchActive() const noexcept
    {
        return m_referenceSwitchInput->read() == hal::gpio::Level::High;
    }
    // Call after stopping when holding torque or the physical datum is lost.
    void invalidateReference() noexcept;

    // Immediate abort (no deceleration ramp), also cancels queued commands.
    void stop() noexcept;
    void stopAndWait() noexcept;

    pnm::Result<hal::step::PulseCount> setVelocity(pnm::units::AngularVelocity velocity);

    pnm::units::Angle position() const;
    pnm::units::AngularVelocity velocity() const;

    pnm::Result<pnm::units::Angle> actualPosition() const noexcept;
    pnm::Result<pnm::units::AngularVelocity> actualVelocity() const noexcept;
    using FeedbackSource = hal::device::IMotorFeedback::Source;
    FeedbackSource feedbackSource() const noexcept
    {
        return m_feedback->source();
    }
    pnm::units::Angle feedbackResolution() const noexcept
    {
        return m_feedback->resolution();
    }

  private:
    friend class control::MotionController;
    // Used only while the controller owns all axes; independent workers are
    // joined before preparing. Accounting still uses the normal callbacks.
    bool prepareCoordinated(bool forward, std::shared_ptr<const hal::step::Sequence> sequence);
    bool coordinatedBlocked() const noexcept;
    std::future<Result> startMotion(std::packaged_task<Result(std::stop_token)> task);

    struct Command
    {
        pnm::units::Angle position;
        pnm::units::AngularVelocity velocity;
        MotionDefaults dynamics;
        BufferMode mode;
        pnm::units::Time timeout;
        bool relative;
        std::promise<Result> completion;
    };
    struct VelocityRequest
    {
        pnm::units::AngularVelocity velocity;
        std::promise<pnm::Result<hal::step::PulseCount>> completion;
    };
    std::future<Result> submitMove(pnm::units::Angle position,
                                   pnm::units::AngularVelocity velocity,
                                   MotionDefaults overrides,
                                   BufferMode mode,
                                   pnm::units::Time timeout,
                                   bool relative);
    void runCommands(std::shared_ptr<Command> command, std::stop_token stop) noexcept;
    void finishQueue(Result result) noexcept;

    Result performMotion(Direction direction,
                         pnm::units::AngularVelocity velocity,
                         pnm::units::Time timeout,
                         std::stop_token stop,
                         std::optional<hal::step::PulseCount> count = std::nullopt,
                         std::optional<hal::gpio::Level> switch_target = std::nullopt);

    Result performReference(pnm::units::AngularVelocity seek_velocity,
                            pnm::units::AngularVelocity latch_velocity,
                            pnm::units::Time timeout,
                            std::stop_token stop);
    Result waitReferenceLevel(hal::gpio::Level level,
                              std::chrono::steady_clock::time_point deadline,
                              std::stop_token stop);
    Result applyReferencePosition(std::stop_token stop);

    std::optional<hal::step::Timing> timingFor(pnm::units::AngularVelocity velocity) const noexcept;
    void accountProgress(const hal::step::AxisStatus& status) noexcept;
    void accountFeedback(const hal::device::IMotorFeedback::Sample& sample) noexcept;

    hal::board::MotorId m_id;
    pnm::units::Angle m_referenceSwitchPosition;
    pnm::units::Angle m_fullStepAngle;
    size_t m_microsteps;
    pnm::units::Angle m_stepAngle;

    struct MotionEvents
    {
        runtime::Notification notification;
        std::atomic_bool referenceActivated{};
        std::atomic_bool referenceReleased{};
        std::atomic_uint32_t referenceChanges{};
        // Installed once before the generator starts; owned by MotionController.
        std::atomic<runtime::Notification*> groupNotification{};
    };
    std::shared_ptr<MotionEvents> m_events{ std::make_shared<MotionEvents>() };

    std::shared_ptr<hal::IStepOutput> m_stepOutput;
    std::shared_ptr<hal::IDigitalOutput> m_dirOutput;
    std::shared_ptr<hal::IDigitalInput> m_referenceSwitchInput;
    std::shared_ptr<hal::device::IMotorFeedback> m_feedback;
    std::shared_ptr<hal::device::IStepperDriver> m_driver;

    mutable std::mutex m_mutex;
    std::mutex m_workerMutex;
    std::jthread m_worker;
    mutable std::mutex m_queueMutex;
    MotionDefaults m_motionDefaults;
    bool m_busy{}, m_homing{};
    std::shared_ptr<Command> m_pending;
    std::shared_ptr<VelocityRequest> m_velocityRequest;
    std::atomic_bool m_profileActive{};
    std::atomic<hal::step::PulseCount> m_wakeAtPulse{ std::numeric_limits<hal::step::PulseCount>::max() };

    double m_motionSign{ 1.0 };
    hal::step::PulseCount m_accountedPulses{ 0U };

    std::atomic_bool m_referenced{ false };
    std::atomic_bool m_referencing{ false };
    std::atomic<pnm::units::Angle> m_position{ 0_deg };
    std::atomic<pnm::units::AngularVelocity> m_velocity{ 0_rpm };
    std::atomic_bool m_feedbackHealthy{ false };
    std::atomic_bool m_feedbackReferenceLost{ false };
    std::atomic<std::errc> m_feedbackError{ std::errc::state_not_recoverable };
    std::atomic<pnm::units::Angle> m_actualPosition{ 0_deg };
    std::atomic<pnm::units::AngularVelocity> m_actualVelocity{ 0_rpm };
};
