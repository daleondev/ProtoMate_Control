#include "StepperMotor.hpp"

#include "pneumo/logging.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

StepperMotor::StepperMotor(hal::board::MotorId id,
                           pnm::units::Angle full_step_angle,
                           size_t microsteps,
                           const std::shared_ptr<hal::IStepGenerator>& step_generator)
  : m_id{ id }
  , m_fullStepAngle{ full_step_angle }
  , m_microsteps{ microsteps }
  , m_stepOutput{ hal::board::createStepperStepOutput(step_generator, id) }
  , m_dirOutput{ hal::board::createStepperDirectionOutput(id) }
  , m_referenceSwitchInput{ hal::board::createReferenceLimitSwitch(id) }
  , m_encoderInput{ hal::board::createEncoder(id) }
  , m_encoderIndexInput{ hal::board::createEncoderIndex(id) }
{
    if (!full_step_angle.isFinite() || full_step_angle <= 0_deg || microsteps == 0U) {
        throw std::invalid_argument("invalid motor step angle or microstep count");
    }
    m_stepAngle = m_fullStepAngle / static_cast<double>(m_microsteps);

    if (!m_stepOutput || !m_dirOutput || !m_referenceSwitchInput ||
        (id == hal::board::MotorId::Motor1 && (!m_encoderInput || !m_encoderIndexInput))) {
        throw std::runtime_error("motor board resource is unavailable");
    }

    pnm::log::debug("Motor initialized: {} degrees per microstep",
                    m_stepAngle.get<pnm::units::AngleUnits::deg>());
}

StepperMotor::~StepperMotor() { stopAndWait(); }

std::future<StepperMotor::Result> StepperMotor::move(Direction direction,
                                                     pnm::units::AngularVelocity velocity,
                                                     pnm::units::Time timeout)
{
    pnm::log::debug("Motor move requested: direction={}, velocity={} rpm, timeout={} s",
                    direction,
                    velocity.get<pnm::units::AngularVelocityUnits::rpm>(),
                    timeout.get<pnm::units::TimeUnits::s>());

    return startMotion(
      std::packaged_task<Result(std::stop_token)>([this, direction, velocity, timeout](std::stop_token stop) {
        return performMotion(direction, velocity, timeout, stop);
    }));
}

std::future<StepperMotor::Result> StepperMotor::moveRel(pnm::units::Angle distance,
                                                        pnm::units::AngularVelocity velocity,
                                                        pnm::units::Time timeout)
{
    pnm::log::debug("Relative motor move requested: distance={} °, velocity={} rpm, timeout={} s",
                    distance.get<pnm::units::AngleUnits::deg>(),
                    velocity.get<pnm::units::AngularVelocityUnits::rpm>(),
                    timeout.get<pnm::units::TimeUnits::s>());

    return startMotion(
      std::packaged_task<Result(std::stop_token)>([this, distance, velocity, timeout](std::stop_token stop) {
        return performMotion(position() + distance, velocity, timeout, stop);
    }));
}

std::future<StepperMotor::Result> StepperMotor::moveAbs(pnm::units::Angle target,
                                                        pnm::units::AngularVelocity velocity,
                                                        pnm::units::Time timeout)
{
    pnm::log::debug("Absolute motor move requested: target={} °, velocity={} rpm, timeout={} s",
                    target.get<pnm::units::AngleUnits::deg>(),
                    velocity.get<pnm::units::AngularVelocityUnits::rpm>(),
                    timeout.get<pnm::units::TimeUnits::s>());

    return startMotion(
      std::packaged_task<Result(std::stop_token)>([this, target, velocity, timeout](std::stop_token stop) {
        return performMotion(target, velocity, timeout, stop);
    }));
}

pnm::units::Angle StepperMotor::position() const
{
    std::scoped_lock lock{ m_mutex };
    return m_position;
}

void StepperMotor::stop() noexcept
{
    std::scoped_lock worker_lock{ m_workerMutex };
    m_worker.request_stop();

    std::scoped_lock state_lock{ m_mutex };
    static_cast<void>(m_stepOutput->stop());
}

void StepperMotor::stopAndWait() noexcept
{
    std::scoped_lock worker_lock{ m_workerMutex };
    m_worker.request_stop();

    {
        std::scoped_lock state_lock{ m_mutex };
        static_cast<void>(m_stepOutput->stop());
    }

    if (m_worker.joinable()) {
        m_worker.join();
    }
}

std::optional<hal::step::Timing> StepperMotor::timingFor(pnm::units::AngularVelocity velocity) const noexcept
{
    if (!velocity.isFinite() || velocity <= 0_rpm) {
        return std::nullopt;
    }

    auto time{ m_stepAngle / velocity };
    if (!time.isFinite() || time < 0.000010_s || time > 53.6870911s) {
        return std::nullopt;
    }

    const double seconds{ time.get<pnm::units::TimeUnits::s>() };
    return hal::step::Timing{
        .period = std::chrono::nanoseconds{ static_cast<std::int64_t>(std::ceil(seconds * 1e9)) },
        .high_time = 5us,
    };
}

util::Result<hal::step::PulseCount> StepperMotor::setVelocity(pnm::units::AngularVelocity velocity)
{
    const auto timing{ timingFor(velocity) };
    if (!timing) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    std::scoped_lock worker_lock{ m_workerMutex, m_mutex };
    const auto result{ m_stepOutput->updateTiming(*timing) };
    if (result) {
        m_velocity = velocity;
    }
    return result;
}

std::future<StepperMotor::Result> StepperMotor::startMotion(std::packaged_task<Result(std::stop_token)> task)
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

StepperMotor::Result StepperMotor::performMotion(pnm::units::Angle target,
                                                 pnm::units::AngularVelocity velocity,
                                                 pnm::units::Time timeout,
                                                 std::stop_token stop)
{
    const auto distance{ target - position() };
    const long double pulses{ std::round(std::abs(static_cast<long double>(distance / m_stepAngle))) };
    if (!target.isFinite() || !std::isfinite(pulses) ||
        pulses >= static_cast<long double>(std::numeric_limits<hal::step::PulseCount>::max())) {
        return Result::Rejected;
    }
    return performMotion(distance >= 0_deg ? Direction::Forward : Direction::Backward,
                         velocity,
                         timeout,
                         stop,
                         static_cast<hal::step::PulseCount>(pulses));
}

StepperMotor::Result StepperMotor::performMotion(Direction direction,
                                                 pnm::units::AngularVelocity velocity,
                                                 pnm::units::Time timeout,
                                                 std::stop_token stop,
                                                 std::optional<hal::step::PulseCount> count,
                                                 std::move_only_function<bool() noexcept> should_stop)
{
    const auto timing{ timingFor(velocity) };
    if (!timing || !timeout.isFinite() || timeout < 0_s) {
        return Result::Rejected;
    }

    const auto now{ std::chrono::steady_clock::now() };
    const auto available{ (std::chrono::steady_clock::time_point::max() - now) / 2 };
    if (timeout.toChrono<std::chrono::nanoseconds>() >= available) {
        return Result::Rejected;
    }

    const auto deadline{ timeout == 0_s ? std::chrono::steady_clock::time_point::max()
                                        : now + timeout.toChrono<std::chrono::steady_clock::duration>() };
    if (stop.stop_requested()) {
        return Result::Stopped;
    }
    if (count == 0U) {
        return Result::Completed;
    }
    if (!m_stepOutput->prepare(*timing, count)) {
        return Result::Rejected;
    }

    pnm::units::Angle start_position;
    {
        std::scoped_lock lock{ m_mutex };
        if (stop.stop_requested()) {
            return Result::Stopped;
        }

        m_dirOutput->write(direction == Direction::Forward ? hal::gpio::Level::High : hal::gpio::Level::Low);
        if (!m_stepOutput->start()) {
            return Result::Rejected;
        }

        start_position = m_position;
        m_velocity = velocity;
    }

    const double sign{ direction == Direction::Forward ? 1.0 : -1.0 };
    auto account = [&](const hal::step::AxisStatus& status, bool ended) {
        std::scoped_lock lock{ m_mutex };
        if (status.counts_exact) {
            m_position = start_position + m_stepAngle * (sign * static_cast<double>(status.pulses));
        }
        else {
            m_referenced = false;
        }
        if (ended) {
            m_velocity = 0_rpm;
        }
    };

    auto result{ Result::Stopped };
    try {
        while (true) {
            const auto status{ m_stepOutput->status() };
            account(status, false);

            if (status.state == hal::step::State::Underrun || status.state == hal::step::State::DmaError ||
                !status.counts_exact) {
                result = Result::Faulted;
                break;
            }
            if (status.state == hal::step::State::Completed) {
                result = Result::Completed;
                break;
            }
            if (stop.stop_requested() || status.state == hal::step::State::Stopped)
                break;
            if (status.state != hal::step::State::Running) {
                result = Result::Faulted;
                break;
            }
            if (should_stop && should_stop()) {
                result = Result::Completed;
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                result = Result::TimedOut;
                break;
            }
            pnm::utils::concurrent::sleep_for(1ms, stop);
        }
    } catch (...) {
        account(m_stepOutput->stop(), true);
        throw;
    }

    const auto final{ m_stepOutput->stop() };
    account(final, true);

    if (!final.counts_exact || final.state == hal::step::State::DmaError ||
        final.state == hal::step::State::Underrun) {
        result = Result::Faulted;
    }

    pnm::log::debug(
      "Motor motion ended: {}, position={} deg", result, position().get<pnm::units::AngleUnits::deg>());
    return result;
}
