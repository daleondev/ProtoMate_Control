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

    if (!m_stepOutput->setProgressCallback(
          [this](const hal::step::AxisStatus& status) noexcept { accountProgress(status); })) {
        throw std::runtime_error("motor progress subscription unavailable");
    }

    pnm::log::debug("Motor initialized: {} degrees per microstep",
                    m_stepAngle.get<pnm::units::AngleUnits::deg>());
}

StepperMotor::~StepperMotor()
{
    stopAndWait();
    static_cast<void>(m_stepOutput->setProgressCallback(nullptr));
}

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
    static_cast<void>(m_stepOutput->status()); // Refreshes the same fields via accountProgress.
    return m_position.load();
}

pnm::units::AngularVelocity StepperMotor::velocity() const
{
    std::scoped_lock lock{ m_mutex };
    static_cast<void>(m_stepOutput->status());
    return m_velocity.load();
}

void StepperMotor::accountProgress(const hal::step::AxisStatus& status) noexcept
{
    if (status.state == hal::step::State::Running && status.pulses == 0U)
        m_accountedPulses = 0U; // Synchronous start notification establishes each new run.
    if (status.counts_exact) {
        const auto delta{ status.pulses - m_accountedPulses };
        m_position.store(m_position.load() + m_stepAngle * (m_motionSign * static_cast<double>(delta)));
        m_accountedPulses = status.pulses;
    }
    else {
        m_referenced.store(false);
    }
    m_velocity.store(status.state == hal::step::State::Running && status.period.count() > 0
                       ? m_motionSign * m_stepAngle / pnm::units::Time{ status.period }
                       : 0_rpm);
    if (status.state != hal::step::State::Running)
        m_notification.signal(); // State is final before the worker can resume.
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

pnm::Result<hal::step::PulseCount> StepperMotor::setVelocity(pnm::units::AngularVelocity velocity)
{
    const auto timing{ timingFor(velocity) };
    if (!timing) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    std::scoped_lock worker_lock{ m_workerMutex, m_mutex };
    return m_stepOutput->updateTiming(*timing);
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
                                                 std::optional<hal::step::PulseCount> count)
{
    const auto timing{ timingFor(velocity) };
    if (!timing || !timeout.isFinite() || timeout < 0_s) {
        return Result::Rejected;
    }

    const auto now{ std::chrono::steady_clock::now() };
    const auto available{ (std::chrono::steady_clock::time_point::max() - now) / 2 };
    if (timeout >= available) {
        return Result::Rejected;
    }

    const auto deadline{ timeout == 0_s ? std::chrono::steady_clock::time_point::max()
                                        : now + timeout.toChrono<std::chrono::steady_clock::duration>() };
    // The previous worker is joined before reuse. Completion notifications
    // carry no run data: after waking we always read this motion's status.
    m_notification.clear();
    const std::stop_callback cancellation{ stop, [this] { m_notification.signal(); } };
    if (stop.stop_requested()) {
        return Result::Stopped;
    }
    if (count == 0U) {
        return Result::Completed;
    }
    if (!m_stepOutput->prepare(*timing, count)) {
        return Result::Rejected;
    }

    {
        std::scoped_lock lock{ m_mutex };
        if (stop.stop_requested()) {
            return Result::Stopped;
        }

        m_dirOutput->write(direction == Direction::Forward ? hal::gpio::Level::High : hal::gpio::Level::Low);
        m_motionSign = direction == Direction::Forward ? 1.0 : -1.0;
        if (!m_stepOutput->start()) {
            return Result::Rejected;
        }
    }

    auto result{ Result::Stopped };
    try {
        while (true) {
            const auto status{ m_stepOutput->status() };

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
            if (std::chrono::steady_clock::now() >= deadline) {
                result = Result::TimedOut;
                break;
            }
            // Signal-before-wait is retained, so completion/cancellation cannot
            // get lost between the status check and entering the blocked wait.
            static_cast<void>(m_notification.waitUntil(deadline));
        }
    } catch (...) {
        static_cast<void>(m_stepOutput->stop());
        throw;
    }

    const auto final{ m_stepOutput->stop() };

    if (!final.counts_exact || final.state == hal::step::State::DmaError ||
        final.state == hal::step::State::Underrun) {
        result = Result::Faulted;
    }

    pnm::log::debug(
      "Motor motion ended: {}, position={} deg", result, position().get<pnm::units::AngleUnits::deg>());
    return result;
}
