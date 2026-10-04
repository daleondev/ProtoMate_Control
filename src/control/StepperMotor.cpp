#include "StepperMotor.hpp"

#include "pneumo/logging.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

StepperMotor::StepperMotor(hal::board::MotorId id,
                           pnm::units::Angle reference_switch_position,
                           pnm::units::Angle full_step_angle,
                           size_t microsteps,
                           const std::shared_ptr<hal::IStepGenerator>& step_generator)
  : m_id{ id }
  , m_referenceSwitchPosition{ reference_switch_position }
  , m_fullStepAngle{ full_step_angle }
  , m_microsteps{ microsteps }
  , m_stepOutput{ hal::board::createStepperStepOutput(step_generator, id) }
  , m_dirOutput{ hal::board::createStepperDirectionOutput(id) }
  , m_referenceSwitchInput{ hal::board::createReferenceLimitSwitch(id) }
  , m_encoderInput{ hal::board::createEncoder(id) }
  , m_encoderIndexInput{ hal::board::createEncoderIndex(id) }
{
    PNM_ASSERT(m_referenceSwitchInput, "Motor %d has no reference switch", static_cast<int>(id) + 1);

    if (!reference_switch_position.isFinite() || !full_step_angle.isFinite() || full_step_angle <= 0_deg ||
        microsteps == 0U) {
        throw std::invalid_argument("invalid reference position, step angle or microstep count");
    }
    m_stepAngle = m_fullStepAngle / static_cast<double>(m_microsteps);

    if (!m_stepOutput || !m_dirOutput || !m_referenceSwitchInput ||
        (id == hal::board::MotorId::Motor1 && (!m_encoderInput || !m_encoderIndexInput))) {
        throw std::runtime_error("motor board resource is unavailable");
    }

    try {
        if (m_encoderInput) {
            const auto counts{ hal::board::encoderCountsPerRevolution(id) };
            if (counts == 0U) {
                throw std::runtime_error("motor encoder resolution is unavailable");
            }

            m_encoderCountAngle = 360_deg / static_cast<double>(counts);
            m_encoderInput->setSampleCallback(
              [this](const hal::IQuadratureEncoder::Sample& sample) noexcept { accountEncoder(sample); });

            if (!m_encoderInput->start()) {
                throw std::runtime_error("motor encoder could not start");
            }
        }

        if (!m_stepOutput->setProgressCallback(
              [this](const hal::step::AxisStatus& status) noexcept { accountProgress(status); })) {
            throw std::runtime_error("motor progress subscription unavailable");
        }

        static_assert(std::atomic_bool::is_always_lock_free);
        static_assert(std::atomic_uint32_t::is_always_lock_free);
        m_referenceSwitchInput->setEdgeCallback([events = m_events](hal::gpio::Level level) noexcept {
            if (level == hal::gpio::Level::High) {
                events->referenceActivated.store(true);
            }
            else {
                events->referenceReleased.store(true);
            }
            events->referenceChanges.fetch_add(1U);
            events->notification.signal();
        });

        pnm::log::debug("Motor initialized: {} degrees per microstep",
                        m_stepAngle.get<pnm::units::AngleUnits::deg>());
    } catch (...) {
        m_referenceSwitchInput->clearEdgeCallback();
        if (m_encoderInput) {
            m_encoderInput->clearSampleCallback();
            static_cast<void>(m_encoderInput->stop());
        }
        static_cast<void>(m_stepOutput->setProgressCallback(nullptr));
        throw;
    }
}

StepperMotor::~StepperMotor()
{
    stopAndWait();
    if (m_encoderInput) {
        static_cast<void>(m_encoderInput->stop());
        m_encoderInput->clearSampleCallback();
    }
    m_referenceSwitchInput->clearEdgeCallback();
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
    if (!isReferenced()) {
        std::promise<Result> rejected;
        rejected.set_value(Result::Rejected);
        return rejected.get_future();
    }

    pnm::log::debug("Absolute motor move requested: target={} °, velocity={} rpm, timeout={} s",
                    target.get<pnm::units::AngleUnits::deg>(),
                    velocity.get<pnm::units::AngularVelocityUnits::rpm>(),
                    timeout.get<pnm::units::TimeUnits::s>());

    return startMotion(
      std::packaged_task<Result(std::stop_token)>([this, target, velocity, timeout](std::stop_token stop) {
        if (!isReferenced()) {
            return Result::Rejected;
        }
        return performMotion(target, velocity, timeout, stop);
    }));
}

std::future<StepperMotor::Result> StepperMotor::reference(pnm::units::AngularVelocity seek_velocity,
                                                          pnm::units::AngularVelocity latch_velocity,
                                                          pnm::units::Time timeout)
{
    return startMotion(std::packaged_task<Result(std::stop_token)>(
      [this, seek_velocity, latch_velocity, timeout](std::stop_token stop) {
        return performReference(seek_velocity, latch_velocity, timeout, stop);
    }));
}

bool StepperMotor::isReferenced() const noexcept { return m_referenced.load(); }

pnm::units::Angle StepperMotor::position() const
{
    std::scoped_lock lock{ m_mutex };
    static_cast<void>(m_stepOutput->status());
    return m_position.load();
}

pnm::units::AngularVelocity StepperMotor::velocity() const
{
    std::scoped_lock lock{ m_mutex };
    static_cast<void>(m_stepOutput->status());
    return m_velocity.load();
}

pnm::Result<pnm::units::Angle> StepperMotor::actualPosition() const noexcept
{
    if (!m_encoderInput) {
        return std::unexpected(std::make_error_code(std::errc::no_such_device));
    }

    const auto position{ m_actualPosition.load() };
    if (!m_encoderHealthy.load()) {
        return std::unexpected(std::make_error_code(std::errc::state_not_recoverable));
    }

    return position;
}

pnm::Result<pnm::units::AngularVelocity> StepperMotor::actualVelocity() const noexcept
{
    if (!m_encoderInput) {
        return std::unexpected(std::make_error_code(std::errc::no_such_device));
    }

    const auto velocity{ m_actualVelocity.load() };
    if (!m_encoderHealthy.load()) {
        return std::unexpected(std::make_error_code(std::errc::state_not_recoverable));
    }

    return velocity;
}

void StepperMotor::accountEncoder(const hal::IQuadratureEncoder::Sample& sample) noexcept
{
    if (!sample.position) {
        m_referenced.store(false);
        m_encoderHealthy.store(false);
        m_actualVelocity.store(0_rpm);
        m_previousEncoderSample.reset();
        return;
    }

    auto velocity{ 0_rpm };
    if (sample.running && m_previousEncoderSample && m_previousEncoderSample->running &&
        sample.timestamp > m_previousEncoderSample->timestamp) {
        const auto current{ *sample.position };
        const auto previous{ *m_previousEncoderSample->position };
        const auto magnitude{ current >= previous
                                ? static_cast<std::uint64_t>(current) - static_cast<std::uint64_t>(previous)
                                : static_cast<std::uint64_t>(previous) -
                                    static_cast<std::uint64_t>(current) };
        const auto delta{ (current >= previous ? 1.0 : -1.0) * static_cast<double>(magnitude) };
        velocity = m_encoderCountAngle * delta /
                   pnm::units::Time{ sample.timestamp - m_previousEncoderSample->timestamp };
    }

    m_actualPosition.store(m_encoderPositionOffset +
                           m_encoderCountAngle * static_cast<double>(*sample.position));
    m_actualVelocity.store(velocity);
    m_previousEncoderSample = sample;
    m_encoderHealthy.store(true);
}

void StepperMotor::accountProgress(const hal::step::AxisStatus& status) noexcept
{
    if (status.state == hal::step::State::Running && status.pulses == 0U) {
        m_accountedPulses = 0U;
    }

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

    if (status.state != hal::step::State::Running) {
        m_events->notification.signal();
    }
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
    if (m_referencing.load()) {
        return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }
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

StepperMotor::Result StepperMotor::performReference(pnm::units::AngularVelocity seek_velocity,
                                                    pnm::units::AngularVelocity latch_velocity,
                                                    pnm::units::Time timeout,
                                                    std::stop_token stop)
{
    const auto now{ std::chrono::steady_clock::now() };
    if (!timingFor(seek_velocity) || !timingFor(latch_velocity) || latch_velocity >= seek_velocity ||
        !timeout.isFinite() || timeout <= 0_s ||
        timeout >= (std::chrono::steady_clock::time_point::max() - now) / 2) {
        return Result::Rejected;
    }

    m_referencing.store(true);
    m_referenced.store(false);
    struct FinishReferencing
    {
        std::atomic_bool& flag;
        ~FinishReferencing() { flag.store(false); }
    } finish{ m_referencing };

    const auto deadline{ now + timeout.toChrono<std::chrono::steady_clock::duration>() };
    const std::stop_callback cancellation{ stop, [this] { m_events->notification.signal(); } };
    const auto seek = [&](Direction direction, pnm::units::AngularVelocity speed, hal::gpio::Level level) {
        if (stop.stop_requested()) {
            return Result::Stopped;
        }
        const auto remaining{ deadline - std::chrono::steady_clock::now() };
        if (remaining <= std::chrono::steady_clock::duration::zero()) {
            return Result::TimedOut;
        }
        return performMotion(direction, speed, pnm::units::Time{ remaining }, stop, std::nullopt, level);
    };

    using enum hal::gpio::Level;
    if (m_referenceSwitchInput->read() != High) {
        const auto first{ seek(Direction::Forward, seek_velocity, High) };
        if (first != Result::Completed) {
            return first;
        }
    }

    auto result{ waitReferenceLevel(High, deadline, stop) };
    if (result != Result::Completed) {
        return result;
    }

    result = seek(Direction::Backward, seek_velocity, Low);
    if (result != Result::Completed) {
        return result;
    }

    result = waitReferenceLevel(Low, deadline, stop);
    if (result != Result::Completed) {
        return result;
    }

    result = seek(Direction::Forward, latch_velocity, High);
    if (result != Result::Completed) {
        return result;
    }

    result = waitReferenceLevel(High, deadline, stop);
    if (result != Result::Completed) {
        return result;
    }

    return applyReferencePosition(stop);
}

StepperMotor::Result StepperMotor::waitReferenceLevel(hal::gpio::Level level,
                                                      std::chrono::steady_clock::time_point deadline,
                                                      std::stop_token stop)
{
    constexpr auto settle_time{ 10ms };
    std::optional<std::chrono::steady_clock::time_point> stable_since;
    auto changes{ m_events->referenceChanges.load() };
    while (true) {
        if (stop.stop_requested()) {
            return Result::Stopped;
        }

        const auto now{ std::chrono::steady_clock::now() };
        if (now >= deadline) {
            return Result::TimedOut;
        }

        const auto current_changes{ m_events->referenceChanges.load() };
        const auto current_level{ m_referenceSwitchInput->read() };
        if (current_changes != changes || current_level != level) {
            stable_since.reset();
        }

        if (!stable_since && current_level == level) {
            stable_since = now;
        }

        changes = current_changes;
        if (stable_since && now >= *stable_since + settle_time &&
            m_events->referenceChanges.load() == current_changes) {
            return Result::Completed;
        }

        static_cast<void>(m_events->notification.waitUntil(
          stable_since ? std::min(deadline, *stable_since + settle_time) : deadline));
    }
}

StepperMotor::Result StepperMotor::applyReferencePosition(std::stop_token stop)
{
    std::scoped_lock lock{ m_mutex };
    if (stop.stop_requested()) {
        return Result::Stopped;
    }

    const auto status{ m_stepOutput->status() };
    if (!status.counts_exact || status.state == hal::step::State::DmaError ||
        status.state == hal::step::State::Underrun ||
        m_referenceSwitchInput->read() != hal::gpio::Level::High) {
        return Result::Faulted;
    }

    const auto commit = [&] {
        m_position.store(m_referenceSwitchPosition);
        m_velocity.store(0_rpm);
        m_referenced.store(true);
    };

    if (m_encoderInput) {
        m_encoderInput->clearSampleCallback();

        const auto count{ m_encoderInput->position() };
        const bool cancelled{ stop.stop_requested() };
        if (count && !cancelled) {
            m_encoderPositionOffset =
              m_referenceSwitchPosition - m_encoderCountAngle * static_cast<double>(*count);
            commit();
        }

        m_encoderInput->setSampleCallback(
          [this](const hal::IQuadratureEncoder::Sample& sample) noexcept { accountEncoder(sample); });
        if (cancelled) {
            return Result::Stopped;
        }

        return count && m_referenced.load() ? Result::Completed : Result::Faulted;
    }
    else {
        m_actualPosition.store(m_referenceSwitchPosition);
    }
    commit();
    return Result::Completed;
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
        pnm::log::warn("Motor move rejected: invalid parameters");
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
                                                 std::optional<hal::gpio::Level> switch_target)
{
    const auto timing{ timingFor(velocity) };
    if (!timing || !timeout.isFinite() || timeout < 0_s) {
        pnm::log::warn("Motor move rejected: invalid velocity or timeout");
        return Result::Rejected;
    }

    const auto now{ std::chrono::steady_clock::now() };
    const auto available{ (std::chrono::steady_clock::time_point::max() - now) / 2 };
    if (timeout >= available) {
        pnm::log::warn("Motor move rejected: invalid timeout={} s", timeout.get<pnm::units::TimeUnits::s>());
        return Result::Rejected;
    }

    const auto deadline{ timeout == 0_s ? std::chrono::steady_clock::time_point::max()
                                        : now + timeout.toChrono<std::chrono::steady_clock::duration>() };

    m_events->notification.clear();
    const std::stop_callback cancellation{ stop, [this] { m_events->notification.signal(); } };

    if (stop.stop_requested()) {
        return Result::Stopped;
    }
    if (count == 0U) {
        return Result::Completed;
    }

    m_events->referenceActivated.store(false);
    m_events->referenceReleased.store(false);
    const bool toward_reference{ direction == Direction::Forward };
    const auto target_reached = [&] {
        return switch_target &&
               (m_referenceSwitchInput->read() == *switch_target ||
                (*switch_target == hal::gpio::Level::High ? m_events->referenceActivated.load()
                                                          : m_events->referenceReleased.load()));
    };

    if (!m_stepOutput->prepare(*timing, count)) {
        pnm::log::warn("Motor move rejected: invalid timing");
        return Result::Rejected;
    }

    {
        std::scoped_lock lock{ m_mutex };
        if (stop.stop_requested()) {
            return Result::Stopped;
        }

        if (switch_target && target_reached()) {
            return Result::Completed;
        }

        if (!switch_target && toward_reference &&
            (m_events->referenceActivated.load() ||
             m_referenceSwitchInput->read() == hal::gpio::Level::High)) {
            pnm::log::warn("Motor move rejected: reference limit switch active");
            return Result::Rejected;
        }

        m_dirOutput->write(direction == Direction::Forward ? hal::gpio::Level::High : hal::gpio::Level::Low);
        m_motionSign = direction == Direction::Forward ? 1.0 : -1.0;
        if (!m_stepOutput->start()) {
            pnm::log::warn("Motor move rejected: step generation failed");
            return Result::Rejected;
        }
    }

    auto result{ Result::Stopped };
    try {
        while (true) {
            if (!switch_target && toward_reference && m_events->referenceActivated.load()) {
                break;
            }
            const auto status{ m_stepOutput->status() };

            if (status.state == hal::step::State::Underrun || status.state == hal::step::State::DmaError ||
                !status.counts_exact) {
                pnm::log::warn("Motor move faulted: {}", status.state);
                result = Result::Faulted;
                break;
            }
            if (status.state == hal::step::State::Completed) {
                result = Result::Completed;
                break;
            }
            if (stop.stop_requested() || status.state == hal::step::State::Stopped) {
                break;
            }
            if (status.state != hal::step::State::Running) {
                pnm::log::warn("Motor move faulted: {}", status.state);
                result = Result::Faulted;
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                pnm::log::warn("Motor move timed out");
                result = Result::TimedOut;
                break;
            }
            if (target_reached()) {
                result = Result::Completed;
                break;
            }

            static_cast<void>(m_events->notification.waitUntil(deadline));
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
