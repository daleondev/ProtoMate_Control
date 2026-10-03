#include "StepperMotor.hpp"

#include "hal/drivers/factory/encoder.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/factory/pwm.hpp"

#include "pneumo/logging.hpp"

StepperMotor::StepperMotor(pnm::units::Angle full_step_angle, size_t microsteps)
  : m_full_step_angle{ full_step_angle }
  , m_microsteps{ microsteps }
{
    pnm::log::debug("Motor initialized: {} degrees per microstep",
                    m_step_angle.get<pnm::units::AngleUnits::deg>());
}

StepperMotor::~StepperMotor()
{
    pnm::log::debug("Shutting down motor worker");
    std::scoped_lock lock{ m_workerMutex };
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
    pnm::log::debug("Motor shutdown complete");
}

std::future<StepperMotor::Result> StepperMotor::move(Direction direction,
                                                     pnm::units::AngularVelocity velocity,
                                                     pnm::units::Time timeout)
{
    pnm::log::debug("Motor move requested: direction={}, velocity={} rpm, timeout={} s",
                    direction,
                    velocity.get<pnm::units::AngularVelocityUnits::rpm>(),
                    timeout.get<pnm::units::TimeUnits::s>());

    return start_motion(
      std::packaged_task<Result(std::stop_token)>([this, direction, velocity, timeout](std::stop_token stop) {
        return perform_motion(direction, velocity, timeout, stop);
    }));
}

void StepperMotor::stop() noexcept
{
    std::scoped_lock lock{ m_workerMutex };
    if (m_worker.request_stop()) {
        pnm::log::trace("Motor stop requested");
    }
}

void StepperMotor::stopAndWait() noexcept
{
    std::scoped_lock lock{ m_workerMutex };
    m_worker.request_stop();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

std::future<StepperMotor::Result> StepperMotor::start_motion(std::packaged_task<Result(std::stop_token)> task)
{
    std::scoped_lock lock{ m_workerMutex };
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }

    auto future{ task.get_future() };
    m_worker = std::jthread(std::move(task));
    return future;
}

StepperMotor::Result StepperMotor::perform_motion(Direction direction,
                                                  pnm::units::AngularVelocity velocity,
                                                  pnm::units::Time timeout,
                                                  std::stop_token stop,
                                                  std::move_only_function<bool() noexcept> should_stop)
try {

    if (!std::isfinite(velocity.get()) || velocity <= 0.0_rpm || !std::isfinite(timeout.get()) ||
        timeout < 0_s) {
        pnm::log::warn("Motor move rejected: invalid velocity={} rpm or timeout={} s",
                       velocity.get<pnm::units::AngularVelocityUnits::rpm>(),
                       timeout.get<pnm::units::TimeUnits::s>());
        return Result::Rejected;
    }

    constexpr auto minimum_pulse{ 1us };
    auto period_time{ (m_step_angle / velocity).toChrono<std::chrono::nanoseconds>() };
    auto timeout_time{ timeout.toChrono<std::chrono::nanoseconds>() };

    auto start{ std::chrono::steady_clock::now() };
    auto maximum_delay{ (std::chrono::steady_clock::time_point::max() - start) / 2 };

    if (!std::isfinite(period_time.count()) || period_time < 2 * minimum_pulse ||
        period_time >= maximum_delay || timeout_time >= maximum_delay) {
        pnm::log::warn("Motor move rejected: pulse period or timeout outside supported timing range");
        return Result::Rejected;
    }

    auto deadline{ timeout == 0_s
                     ? std::chrono::steady_clock::time_point::max()
                     : start + std::chrono::ceil<std::chrono::steady_clock::duration>(timeout_time) };

    m_dirOutput->write(static_cast<hal::gpio::Level>(direction == Direction::Forward));

    if (!m_stepOutput->configure({ .period = period_time, .high_time = period_time / 2 })) {
        pnm::log::warn("Motor move rejected: failed to configure pwm");
        return Result::Rejected;
    }

    if (!m_stepOutput->start()) {
        pnm::log::warn("Motor move rejected: failed to start pwm");
        return Result::Rejected;
    }

    {
        std::scoped_lock lock{ m_mutex };
        m_velocity = velocity;
    }

    auto result{ Result::Stopped };
    while (true) {

        const auto now{ std::chrono::steady_clock::now() };
        if (stop.stop_requested()) {
            break;
        }
        if (should_stop && should_stop()) {
            result = Result::Completed;
            break;
        }
        if (now >= deadline || deadline - now < minimum_pulse) {
            result = Result::TimedOut;
            break;
        }

        pnm::utils::concurrent::sleep_for(minimum_pulse, stop);
    }
    static_cast<void>(m_stepOutput->stop());

    {
        std::scoped_lock lock{ m_mutex };
        m_velocity = 0_rpm;
    }

    pnm::log::debug(
      "Motor motion ended: {}, position={} deg", result, m_position.get<pnm::units::AngleUnits::deg>());
    if (result == Result::TimedOut && should_stop) {
        pnm::log::warn("Motor timed out before reaching its target after {} s",
                       timeout.get<pnm::units::TimeUnits::s>());
    }
    return result;
} catch (...) {
    {
        std::scoped_lock lock{ m_mutex };
        m_velocity = 0_rpm;
    }
    static_cast<void>(m_stepOutput->stop());
    pnm::log::error(pnm::log::immediate, "Motor motion failed; propagating exception through its future");
    throw;
}