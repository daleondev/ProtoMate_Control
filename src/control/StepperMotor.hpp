#pragma once

#include "pneumo/units.hpp"

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
        Rejected
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
                         std::move_only_function<bool() noexcept> should_stop = nullptr);

    hal::board::MotorId m_id;
    pnm::units::Angle m_fullStepAngle;
    size_t m_microsteps;
    pnm::units::Angle m_stepAngle{ m_fullStepAngle / m_microsteps };

    std::shared_ptr<hal::IStepOutput> m_stepOutput;
    std::shared_ptr<hal::IDigitalOutput> m_dirOutput;
    std::shared_ptr<hal::IDigitalInput> m_referenceSwitchInput;
    std::shared_ptr<hal::IQuadratureEncoder> m_encoderInput;
    std::shared_ptr<hal::IDigitalInput> m_encoderIndexInput;

    mutable std::mutex m_mutex;
    std::mutex m_workerMutex;
    std::jthread m_worker;

    bool m_referenced{ false };
    pnm::units::Angle m_position{ 0_deg };
    pnm::units::AngularVelocity m_velocity{ 0_rpm };
    pnm::units::Angle m_actualPosition{ 0_deg };           // todo: encoder
    pnm::units::AngularVelocity m_actualVelocity{ 0_rpm }; // todo: encoder
};