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
    // Signed commanded state, updated by axis progress and refreshed on read.
    // Velocity reflects the emitted pulse timing, not pending timing changes.
    pnm::units::Angle position() const;
    pnm::units::AngularVelocity velocity() const;

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

    hal::board::MotorId m_id;
    pnm::units::Angle m_fullStepAngle;
    size_t m_microsteps;
    pnm::units::Angle m_stepAngle{};

    runtime::Notification m_notification;
    std::shared_ptr<hal::IStepOutput> m_stepOutput;
    std::shared_ptr<hal::IDigitalOutput> m_dirOutput;
    std::shared_ptr<hal::IDigitalInput> m_referenceSwitchInput;
    std::shared_ptr<hal::IQuadratureEncoder> m_encoderInput;
    std::shared_ptr<hal::IDigitalInput> m_encoderIndexInput;

    mutable std::mutex m_mutex;
    std::mutex m_workerMutex;
    std::jthread m_worker;

    // Set before starting a run; count bookkeeping is owned exclusively by
    // the serialized HAL progress callback. No duplicate position cache.
    double m_motionSign{ 1.0 };
    hal::step::PulseCount m_accountedPulses{};

    std::atomic_bool m_referenced{ false };
    // The runtime's 64-bit atomics use short interrupt-masked accesses on
    // STM32; callbacks never take a thread mutex or block.
    std::atomic<pnm::units::Angle> m_position{ 0_deg };
    std::atomic<pnm::units::AngularVelocity> m_velocity{ 0_rpm };
    pnm::units::Angle m_actualPosition{ 0_deg };           // todo: encoder
    pnm::units::AngularVelocity m_actualVelocity{ 0_rpm }; // todo: encoder
};
