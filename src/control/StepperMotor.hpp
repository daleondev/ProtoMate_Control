#pragma once

#include "pneumo/units.hpp"
#include "runtime/synchronization/Notification.hpp"

#include <atomic>
#include <future>
#include <mutex>
#include <optional>
#include <thread>

#include "hal/board/board.hpp"

using namespace pnm::units::literals;

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

    StepperMotor(hal::board::MotorId id,
                 pnm::units::Angle full_step_angle,
                 size_t microsteps,
                 const std::shared_ptr<hal::IStepGenerator>& step_generator);
    ~StepperMotor();

    StepperMotor(const StepperMotor&) = delete;
    StepperMotor& operator=(const StepperMotor&) = delete;
    StepperMotor(StepperMotor&&) = delete;
    StepperMotor& operator=(StepperMotor&&) = delete;

    std::future<Result> move(Direction direction,
                             pnm::units::AngularVelocity velocity,
                             pnm::units::Time timeout = 0_s);

    std::future<Result> moveRel(pnm::units::Angle distance,
                                pnm::units::AngularVelocity velocity,
                                pnm::units::Time timeout = 0_s);

    std::future<Result> moveAbs(pnm::units::Angle target,
                                pnm::units::AngularVelocity velocity,
                                pnm::units::Time timeout = 0_s);

    void stop() noexcept;
    void stopAndWait() noexcept;

    pnm::Result<hal::step::PulseCount> setVelocity(pnm::units::AngularVelocity velocity);

    pnm::units::Angle position() const;
    pnm::units::AngularVelocity velocity() const;

    // Latest encoder measurements, updated by timed callbacks even while STEP
    // is stopped. Relative to construction, not a homed absolute position.
    // No encoder: no_such_device. Invalid count: state_not_recoverable.
    pnm::Result<pnm::units::Angle> actualPosition() const noexcept;
    pnm::Result<pnm::units::AngularVelocity> actualVelocity() const noexcept;

  private:
    std::future<Result> startMotion(std::packaged_task<Result(std::stop_token)> task);

    Result performMotion(pnm::units::Angle position,
                         pnm::units::AngularVelocity velocity,
                         pnm::units::Time timeout,
                         std::stop_token stop);

    Result performMotion(Direction direction,
                         pnm::units::AngularVelocity velocity,
                         pnm::units::Time timeout,
                         std::stop_token stop,
                         std::optional<hal::step::PulseCount> count = std::nullopt);

    std::optional<hal::step::Timing> timingFor(pnm::units::AngularVelocity velocity) const noexcept;
    void accountProgress(const hal::step::AxisStatus& status) noexcept;
    void accountEncoder(const hal::IQuadratureEncoder::Sample& sample) noexcept;

    hal::board::MotorId m_id;
    pnm::units::Angle m_fullStepAngle;
    size_t m_microsteps;
    pnm::units::Angle m_stepAngle;

    struct MotionEvents
    {
        runtime::Notification notification;
        std::atomic_bool referenceActivated{};
    };
    std::shared_ptr<MotionEvents> m_events{ std::make_shared<MotionEvents>() };

    std::shared_ptr<hal::IStepOutput> m_stepOutput;
    std::shared_ptr<hal::IDigitalOutput> m_dirOutput;
    std::shared_ptr<hal::IDigitalInput> m_referenceSwitchInput;
    std::shared_ptr<hal::IQuadratureEncoder> m_encoderInput;
    std::shared_ptr<hal::IDigitalInput> m_encoderIndexInput;

    mutable std::mutex m_mutex;
    std::mutex m_workerMutex;
    std::jthread m_worker;

    double m_motionSign{ 1.0 };
    hal::step::PulseCount m_accountedPulses{ 0U };

    std::atomic_bool m_referenced{ false };
    std::atomic<pnm::units::Angle> m_position{ 0_deg };
    std::atomic<pnm::units::AngularVelocity> m_velocity{ 0_rpm };
    pnm::units::Angle m_encoderCountAngle{ 0_deg };
    std::optional<hal::IQuadratureEncoder::Sample> m_previousEncoderSample;
    std::atomic_bool m_encoderHealthy{ false };
    std::atomic<pnm::units::Angle> m_actualPosition{ 0_deg };
    std::atomic<pnm::units::AngularVelocity> m_actualVelocity{ 0_rpm };
};
