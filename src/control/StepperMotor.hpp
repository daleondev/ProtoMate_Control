#pragma once

#include "pneumo/units.hpp"

#include <future>
#include <mutex>
#include <optional>
#include <thread>

using namespace pnm::units::literals;

namespace hal
{
    class IDigitalInput;
    class IDigitalOutput;
    class IPwmOutput;
    class IQuadratureEncoder;
};

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

    StepperMotor(pnm::units::Angle full_step_angle, size_t microsteps);
    ~StepperMotor();

    StepperMotor(const StepperMotor&) = delete;
    StepperMotor& operator=(const StepperMotor&) = delete;
    StepperMotor(StepperMotor&&) = delete;
    StepperMotor& operator=(StepperMotor&&) = delete;

    std::future<Result> move(Direction direction,
                             pnm::units::AngularVelocity velocity,
                             pnm::units::Time timeout = 0_s);

    std::future<Result> move_relative(pnm::units::Angle distance,
                                      pnm::units::AngularVelocity velocity,
                                      pnm::units::Time timeout = 0_s);

    std::future<Result> move_absolute(pnm::units::Angle target,
                                      pnm::units::AngularVelocity velocity,
                                      pnm::units::Time timeout = 0_s);

    void stop() noexcept;
    void stopAndWait() noexcept;

  private:
    std::future<Result> start_motion(std::packaged_task<Result(std::stop_token)> task);

    Result perform_motion(pnm::units::Angle position,
                          pnm::units::AngularVelocity velocity,
                          pnm::units::Time timeout,
                          std::stop_token stop);

    Result perform_motion(Direction direction,
                          pnm::units::AngularVelocity velocity,
                          pnm::units::Time timeout,
                          std::stop_token stop,
                          std::move_only_function<bool() noexcept> should_stop = nullptr);

    pnm::units::Angle m_full_step_angle;
    size_t m_microsteps;
    pnm::units::Angle m_step_angle{ m_full_step_angle / m_microsteps };

    std::shared_ptr<hal::IPwmOutput> m_stepOutput;
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