#include "StepperMotor.hpp"
#include "MotionSequence.hpp"

#include "pneumo/logging.hpp"
#include "runtime/thread.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

StepperMotor::StepperMotor(hal::board::MotorId id,
                           pnm::units::Angle reference_switch_position,
                           pnm::units::Angle full_step_angle,
                           size_t microsteps,
                           const std::shared_ptr<hal::IStepGenerator>& step_generator)
  : StepperMotor{ id, reference_switch_position, full_step_angle, microsteps, step_generator,
                  hal::board::createMotorFeedback(id, { full_step_angle, microsteps }) }
{
}

StepperMotor::StepperMotor(hal::board::MotorId id,
                           pnm::units::Angle reference_switch_position,
                           pnm::units::Angle full_step_angle,
                           size_t microsteps,
                           const std::shared_ptr<hal::IStepGenerator>& step_generator,
                           std::shared_ptr<hal::device::IMotorFeedback> feedback)
  : m_id{ id }
  , m_referenceSwitchPosition{ reference_switch_position }
  , m_fullStepAngle{ full_step_angle }
  , m_microsteps{ microsteps }
  , m_stepOutput{ hal::board::createStepperStepOutput(step_generator, id) }
  , m_dirOutput{ hal::board::createStepperDirectionOutput(id) }
  , m_referenceSwitchInput{ hal::board::createReferenceLimitSwitch(id) }
  , m_feedback{ std::move(feedback) }
{
    PNM_ASSERT(m_referenceSwitchInput, "Motor %d has no reference switch", static_cast<int>(id) + 1);

    if (!reference_switch_position.isFinite() || !full_step_angle.isFinite() || full_step_angle <= 0_deg ||
        microsteps == 0U) {
        throw std::invalid_argument("invalid reference position, step angle or microstep count");
    }
    m_stepAngle = m_fullStepAngle / static_cast<double>(m_microsteps);

    if (!m_stepOutput || !m_dirOutput || !m_referenceSwitchInput || !m_feedback) {
        throw std::runtime_error("motor board resource is unavailable");
    }

    try {
        m_feedback->setCallback([this](const hal::device::IMotorFeedback::Sample& sample) noexcept {
            accountFeedback(sample);
        });
        if (!m_feedback->start())
            throw std::runtime_error("motor feedback could not start");

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
            if (auto* notification{ events->groupNotification.load() })
                notification->signal();
        });

        pnm::log::debug("Motor initialized: {} degrees per microstep",
                        m_stepAngle.get<pnm::units::AngleUnits::deg>());
    } catch (...) {
        m_feedback->clearCallback();
        static_cast<void>(m_feedback->stop());
        m_referenceSwitchInput->clearEdgeCallback();
        static_cast<void>(m_stepOutput->setProgressCallback(nullptr));
        throw;
    }
}

StepperMotor::~StepperMotor()
{
    stopAndWait();
    static_cast<void>(m_feedback->stop());
    m_feedback->clearCallback();
    m_referenceSwitchInput->clearEdgeCallback();
    static_cast<void>(m_stepOutput->setProgressCallback(nullptr));
}

std::future<StepperMotor::Result> StepperMotor::moveRel(pnm::units::Angle distance,
                                                        pnm::units::AngularVelocity velocity,
                                                        pnm::units::AngularAcceleration acceleration,
                                                        pnm::units::AngularAcceleration deceleration,
                                                        pnm::units::AngularJerk jerk,
                                                        BufferMode mode,
                                                        pnm::units::Time timeout)
{
    return submitMove(distance, velocity, { acceleration, deceleration, jerk }, mode, timeout, true);
}

std::future<StepperMotor::Result> StepperMotor::moveAbs(pnm::units::Angle target,
                                                        pnm::units::AngularVelocity velocity,
                                                        pnm::units::AngularAcceleration acceleration,
                                                        pnm::units::AngularAcceleration deceleration,
                                                        pnm::units::AngularJerk jerk,
                                                        BufferMode mode,
                                                        pnm::units::Time timeout)
{
    return submitMove(target, velocity, { acceleration, deceleration, jerk }, mode, timeout, false);
}

std::future<StepperMotor::Result> StepperMotor::moveRel(pnm::units::Angle distance,
                                                        pnm::units::AngularVelocity velocity,
                                                        pnm::units::Time timeout)
{
    return moveRel(distance, velocity, 0_rad_s2, 0_rad_s2, 0_rad_s3, BufferMode::Aborting, timeout);
}

std::future<StepperMotor::Result> StepperMotor::moveAbs(pnm::units::Angle target,
                                                        pnm::units::AngularVelocity velocity,
                                                        pnm::units::Time timeout)
{
    return moveAbs(target, velocity, 0_rad_s2, 0_rad_s2, 0_rad_s3, BufferMode::Aborting, timeout);
}

pnm::Result<> StepperMotor::setMotionDefaults(MotionDefaults defaults)
{
    if (!defaults.acceleration.isFinite() || defaults.acceleration <= 0_rad_s2 ||
        !defaults.deceleration.isFinite() || defaults.deceleration <= 0_rad_s2 || !defaults.jerk.isFinite() ||
        defaults.jerk < 0_rad_s3)
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    std::scoped_lock lock{ m_queueMutex };
    if (m_busy)
        return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
    m_motionDefaults = defaults;
    return {};
}

StepperMotor::MotionDefaults StepperMotor::motionDefaults() const
{
    std::scoped_lock lock{ m_queueMutex };
    return m_motionDefaults;
}

std::future<StepperMotor::Result> StepperMotor::submitMove(pnm::units::Angle position,
                                                           pnm::units::AngularVelocity velocity,
                                                           MotionDefaults dynamics,
                                                           BufferMode mode,
                                                           pnm::units::Time timeout,
                                                           bool relative)
{
    const auto rejected = [] {
        std::promise<Result> promise;
        promise.set_value(Result::Rejected);
        return promise.get_future();
    };
    const auto now{ std::chrono::steady_clock::now() };
    if (!position.isFinite() || !timingFor(velocity) || !timeout.isFinite() || timeout < 0_s ||
        timeout >= (std::chrono::steady_clock::time_point::max() - now) / 2 ||
        !dynamics.acceleration.isFinite() || dynamics.acceleration < 0_rad_s2 ||
        !dynamics.deceleration.isFinite() || dynamics.deceleration < 0_rad_s2 || !dynamics.jerk.isFinite() ||
        dynamics.jerk < 0_rad_s3 || mode < BufferMode::Aborting || mode > BufferMode::BlendingHigh ||
        (!relative && !isReferenced()))
        return rejected();
    std::scoped_lock worker_lock{ m_workerMutex };
    {
        std::scoped_lock lock{ m_queueMutex };
        if (dynamics.acceleration == 0_rad_s2)
            dynamics.acceleration = m_motionDefaults.acceleration;
        if (dynamics.deceleration == 0_rad_s2)
            dynamics.deceleration = m_motionDefaults.deceleration;
        if (dynamics.jerk == 0_rad_s3)
            dynamics.jerk = m_motionDefaults.jerk;
    }
    auto command{ std::make_shared<Command>(
      Command{ position, velocity, dynamics, mode, timeout, relative, {} }) };
    auto future{ command->completion.get_future() };
    if (mode != BufferMode::Aborting) {
        std::scoped_lock lock{ m_queueMutex };
        if (m_busy) {
            if (m_homing || m_pending) {
                command->completion.set_value(Result::Rejected);
                return future;
            }
            m_pending = command;
            m_events->notification.signal();
            return future;
        }
    }
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
    {
        std::scoped_lock lock{ m_queueMutex };
        m_busy = true;
        m_homing = false;
    }
    try {
        m_worker = runtime::thread::create_jthread(
          { .name = "motion", .stack_size = 16384U },
          [this, command](std::stop_token stop) { runCommands(command, stop); });
    } catch (...) {
        command->completion.set_exception(std::current_exception());
        finishQueue(Result::Faulted);
    }
    return future;
}

void StepperMotor::runCommands(std::shared_ptr<Command> command, std::stop_token stop) noexcept
{
    using Count = hal::step::PulseCount;
    using Clock = std::chrono::steady_clock;
    using enum hal::step::State;
    const auto step{ m_stepAngle.get() };
    bool resolved{};
    std::shared_ptr<VelocityRequest> velocity_request;
    const auto complete = [&](Result result) {
        if (!resolved) {
            command->completion.set_value(result);
            resolved = true;
        }
    };
    const auto quantize = [&](pnm::units::Angle distance) -> std::optional<Count> {
        const auto pulses{ std::round(std::abs(distance.get() / step)) };
        // Retain exact integer representation while planning in double precision.
        if (!distance.isFinite() || !std::isfinite(pulses) || pulses >= 0x1p52)
            return {};
        return static_cast<Count>(pulses);
    };
    const auto limits = [](const Command& request) {
        return motion::Profile::Limits{ request.velocity.get(),
                                        request.dynamics.acceleration.get(),
                                        request.dynamics.deceleration.get(),
                                        request.dynamics.jerk.get() };
    };
    const auto deadlineFor = [](const Command& request) {
        return request.timeout == 0_s ? Clock::time_point::max()
                                      : Clock::now() + request.timeout.toChrono<Clock::duration>();
    };
    const auto takeNext = [&] {
        std::scoped_lock lock{ m_queueMutex };
        if (m_velocityRequest) {
            m_velocityRequest->completion.set_value(
              std::unexpected(std::make_error_code(std::errc::operation_canceled)));
            m_velocityRequest.reset();
        }
        auto next{ std::exchange(m_pending, {}) };
        if (!next)
            m_busy = false;
        return next;
    };
    const std::stop_callback cancellation{ stop, [this] { m_events->notification.signal(); } };
    try {
        while (command) {
            resolved = false;
            m_wakeAtPulse.store(std::numeric_limits<Count>::max());
            if (stop.stop_requested()) {
                complete(Result::Stopped);
                break;
            }
            if (!command->relative && !isReferenced()) {
                complete(Result::Rejected);
                break;
            }
            const auto origin{ position() };
            const auto distance{ command->relative ? command->position : command->position - origin };
            const auto count{ quantize(distance) };
            if (!count) {
                complete(Result::Rejected);
                break;
            }
            if (*count == 0U) {
                complete(Result::Completed);
                command = takeNext();
                continue;
            }
            const auto sign{ distance.get() >= 0.0 ? 1.0 : -1.0 };
            Count active_end{ *count };
            auto active_target{ origin + m_stepAngle * (sign * static_cast<double>(*count)) };
            auto initial{ motion::Profile::create(static_cast<double>(*count) * step, limits(*command)) };
            if (!initial) {
                complete(Result::Rejected);
                break;
            }
            auto sequence{ std::make_shared<motion::Sequence>(
              step, motion::Sequence::Part{ *initial, 0U, *count }) };
            std::shared_ptr<Command> following;
            auto deadline{ deadlineFor(*command) };

            const auto blend = [&](Count begin,
                                   motion::Profile::State state,
                                   std::optional<hal::step::ScheduleCursor> cursor) {
                std::shared_ptr<Command> next;
                {
                    std::scoped_lock lock{ m_queueMutex };
                    next = m_pending;
                }
                if (following || !next || next->mode == BufferMode::Buffered || begin >= active_end ||
                    (!next->relative && !isReferenced()))
                    return false;
                const auto delta{ next->relative ? next->position : next->position - active_target };
                const auto next_count{ quantize(delta) };
                if (!next_count || *next_count == 0U || delta.get() * sign <= 0.0 ||
                    active_end > static_cast<Count>(0x1p52) - *next_count)
                    return false;
                double junction{};
                switch (next->mode) {
                    case BufferMode::BlendingLow:
                        junction = std::min(command->velocity.get(), next->velocity.get());
                        break;
                    case BufferMode::BlendingPrevious:
                        junction = command->velocity.get();
                        break;
                    case BufferMode::BlendingNext:
                        junction = next->velocity.get();
                        break;
                    case BufferMode::BlendingHigh:
                        junction = std::max(command->velocity.get(), next->velocity.get());
                        break;
                    default:
                        return false;
                }
                const auto make = [&](double speed) -> std::shared_ptr<motion::Sequence> {
                    auto constraints{ limits(*command) };
                    constraints.velocity = std::max(constraints.velocity, speed);
                    const auto first{ motion::Profile::create(static_cast<double>(active_end - begin) * step,
                                                              constraints,
                                                              state.velocity,
                                                              state.acceleration,
                                                              speed) };
                    const auto second{ motion::Profile::create(
                      static_cast<double>(*next_count) * step, limits(*next), speed) };
                    if (!first || !second)
                        return {};
                    return std::make_shared<motion::Sequence>(
                      step,
                      motion::Sequence::Part{ *first, begin, active_end },
                      motion::Sequence::Part{ *second, active_end, active_end + *next_count });
                };
                auto replacement{ make(junction) };
                // Short or late commands may not reach the requested junction.
                // Find a feasible lower speed, then refine its upper boundary.
                if (!replacement) {
                    double upper{ junction };
                    for (unsigned i = 1U; i <= 32U; ++i) {
                        const auto lower{ junction * (1.0 - static_cast<double>(i) / 32.0) };
                        replacement = make(lower);
                        if (replacement) {
                            auto low{ lower }, high{ upper };
                            for (unsigned j = 0U; j < 24U; ++j) {
                                const auto middle{ (low + high) * 0.5 };
                                if (auto candidate{ make(middle) }) {
                                    low = middle;
                                    replacement = std::move(candidate);
                                }
                                else
                                    high = middle;
                            }
                            break;
                        }
                        upper = lower;
                    }
                }
                if (!replacement)
                    return false;
                if (cursor && !m_stepOutput->replaceSequence(*cursor, replacement))
                    return false;
                sequence = std::move(replacement);
                following = std::move(next);
                m_wakeAtPulse.store(active_end);
                return true;
            };

            // Back-to-back submissions can blend before the first DMA buffer is
            // armed, including short moves that fit completely in those buffers.
            static_cast<void>(blend(0U, {}, std::nullopt));
            m_events->notification.clear();
            m_events->referenceActivated.store(false);
            m_events->referenceReleased.store(false);
            if (!m_stepOutput->prepareSequence(sequence)) {
                complete(Result::Rejected);
                break;
            }
            {
                std::scoped_lock lock{ m_mutex };
                if (stop.stop_requested()) {
                    complete(Result::Stopped);
                    break;
                }
                if (sign > 0.0 && (m_events->referenceActivated.load() ||
                                   m_referenceSwitchInput->read() == hal::gpio::Level::High)) {
                    complete(Result::Rejected);
                    break;
                }
                m_dirOutput->write(sign > 0.0 ? hal::gpio::Level::High : hal::gpio::Level::Low);
                m_motionSign = sign;
                m_profileActive.store(true);
                const auto delay{ std::chrono::nanoseconds{
                  static_cast<std::int64_t>(std::ceil((sequence->timeAtPulse(1U) + 0.01) * 1e9)) } };
                if (!m_stepOutput->start(delay)) {
                    complete(Result::Rejected);
                    break;
                }
            }

            auto result{ Result::Stopped };
            while (true) {
                const auto status{ m_stepOutput->status() };
                if (!status.counts_exact || status.state == Underrun || status.state == DmaError) {
                    result = Result::Faulted;
                    break;
                }
                if (following && status.pulses >= active_end) {
                    m_wakeAtPulse.store(std::numeric_limits<Count>::max());
                    const auto previous{ command };
                    const auto old_end{ active_end };
                    active_end = sequence->count();
                    active_target += m_stepAngle * (sign * static_cast<double>(active_end - old_end));
                    {
                        std::scoped_lock lock{ m_queueMutex };
                        m_pending.reset();
                    }
                    command = std::move(following);
                    resolved = false;
                    deadline = deadlineFor(*command);
                    previous->completion.set_value(Result::Completed);
                }
                if (status.state == Completed) {
                    result = Result::Completed;
                    break;
                }
                if (stop.stop_requested() || status.state == Stopped ||
                    (sign > 0.0 && m_events->referenceActivated.load()))
                    break;
                if (status.state != Running || (!command->relative && !isReferenced())) {
                    result = Result::Faulted;
                    break;
                }
                if (Clock::now() >= deadline) {
                    result = Result::TimedOut;
                    break;
                }

                {
                    std::scoped_lock lock{ m_queueMutex };
                    velocity_request = std::exchange(m_velocityRequest, {});
                }
                if (velocity_request) {
                    auto update_result{ pnm::Result<Count>{
                      std::unexpected(std::make_error_code(std::errc::operation_not_permitted)) } };
                    const auto old_velocity{ command->velocity };
                    for (unsigned attempt = 0; attempt < 3U; ++attempt) {
                        const auto cursor{ m_stepOutput->scheduleCursor() };
                        if (!cursor || cursor->terminal || cursor->first_uncommitted + 1U >= active_end)
                            break;
                        const auto begin{ cursor->first_uncommitted + 1U };
                        const auto state{ sequence->atPulse(begin) };
                        command->velocity = velocity_request->velocity;
                        const auto end_speed{ following ? sequence->first().profile.endVelocity() : 0.0 };
                        const auto profile{ motion::Profile::create(static_cast<double>(active_end - begin) *
                                                                      step,
                                                                    limits(*command),
                                                                    state.velocity,
                                                                    state.acceleration,
                                                                    end_speed) };
                        if (!profile)
                            break;
                        auto replacement{ std::make_shared<motion::Sequence>(
                          step,
                          motion::Sequence::Part{ *profile, begin, active_end },
                          following ? sequence->second() : std::nullopt) };
                        const auto applied{ m_stepOutput->replaceSequence(*cursor, replacement) };
                        if (applied) {
                            sequence = std::move(replacement);
                            update_result = begin; // 1-based first pulse with a changed following interval.
                            break;
                        }
                        update_result = std::unexpected(applied.error());
                        if (applied.error() != std::errc::resource_unavailable_try_again)
                            break;
                    }
                    if (!update_result)
                        command->velocity = old_velocity;
                    velocity_request->completion.set_value(update_result);
                    velocity_request.reset();
                }
                if (!following) {
                    for (unsigned attempt = 0U; attempt < 3U && !following; ++attempt) {
                        const auto cursor{ m_stepOutput->scheduleCursor() };
                        if (!cursor || cursor->terminal || cursor->first_uncommitted + 1U >= active_end)
                            break;
                        const auto begin{ cursor->first_uncommitted + 1U };
                        if (blend(begin, sequence->atPulse(begin), *cursor))
                            break;
                        const auto after{ m_stepOutput->scheduleCursor() };
                        if (!after || after->first_uncommitted == cursor->first_uncommitted)
                            break;
                    }
                }
                static_cast<void>(m_events->notification.waitUntil(deadline));
            }
            const auto final{ m_stepOutput->stop() };
            m_profileActive.store(false);
            if (!final.counts_exact || final.state == Underrun || final.state == DmaError)
                result = Result::Faulted;
            complete(result);
            if (result != Result::Completed || stop.stop_requested())
                break;
            command = takeNext();
        }
    } catch (...) {
        if (velocity_request)
            velocity_request->completion.set_value(
              std::unexpected(std::make_error_code(std::errc::not_enough_memory)));
        complete(Result::Faulted);
    }
    static_cast<void>(m_stepOutput->stop());
    finishQueue(Result::Stopped);
}

std::future<StepperMotor::Result> StepperMotor::reference(pnm::units::AngularVelocity seek_velocity,
                                                          pnm::units::AngularVelocity latch_velocity,
                                                          pnm::units::Time timeout)
{
    pnm::log::debug(
      "Motor {} reference requested: seek={} rpm, latch={} rpm, timeout={} s, switch position={} deg",
      static_cast<unsigned>(m_id) + 1U,
      seek_velocity.get<pnm::units::AngularVelocityUnits::rpm>(),
      latch_velocity.get<pnm::units::AngularVelocityUnits::rpm>(),
      timeout.get<pnm::units::TimeUnits::s>(),
      m_referenceSwitchPosition.get<pnm::units::AngleUnits::deg>());

    return startMotion(std::packaged_task<Result(std::stop_token)>(
      [this, seek_velocity, latch_velocity, timeout](std::stop_token stop) {
        const auto result{ performReference(seek_velocity, latch_velocity, timeout, stop) };
        pnm::log::debug("Motor {} reference ended: {}, referenced={}, position={} deg",
                        static_cast<unsigned>(m_id) + 1U,
                        result,
                        isReferenced(),
                        m_position.load().get<pnm::units::AngleUnits::deg>());
        return result;
    }));
}

bool StepperMotor::isReferenced() const noexcept { return m_referenced.load(); }

void StepperMotor::invalidateReference() noexcept
{
    m_referenced.store(false);
    m_feedback->invalidate();
}

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
    if (!m_feedback) {
        return std::unexpected(std::make_error_code(std::errc::no_such_device));
    }

    const auto position{ m_actualPosition.load() };
    if (!m_feedbackHealthy.load()) {
        return std::unexpected(std::make_error_code(m_feedbackError.load()));
    }

    return position;
}

pnm::Result<pnm::units::AngularVelocity> StepperMotor::actualVelocity() const noexcept
{
    if (!m_feedback) {
        return std::unexpected(std::make_error_code(std::errc::no_such_device));
    }

    const auto velocity{ m_actualVelocity.load() };
    if (!m_feedbackHealthy.load()) {
        return std::unexpected(std::make_error_code(m_feedbackError.load()));
    }

    return velocity;
}

void StepperMotor::accountFeedback(const hal::device::IMotorFeedback::Sample& sample) noexcept
{
    m_feedbackReferenceLost.store(sample.reference_lost);
    if (sample.reference_lost) {
        m_referenced.store(false);
        if (auto* notification{ m_events->groupNotification.load() })
            notification->signal();
        m_events->notification.signal();
    }
    if (!sample.position) {
        m_feedbackError.store(static_cast<std::errc>(sample.position.error().value()));
        m_actualVelocity.store(0_rpm);
        m_feedbackHealthy.store(false);
        return;
    }
    m_actualPosition.store(*sample.position);
    m_actualVelocity.store(sample.velocity);
    m_feedbackHealthy.store(true);
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

    if (!status.counts_exact)
        m_feedback->invalidate();
    else
        m_feedback->motion(status.state == hal::step::State::Running, m_motionSign > 0, status.period);

    auto boundary{ m_wakeAtPulse.load() };
    const bool reached{ status.pulses >= boundary &&
                        m_wakeAtPulse.compare_exchange_strong(
                          boundary, std::numeric_limits<hal::step::PulseCount>::max()) };
    if (status.state != hal::step::State::Running || reached) {
        m_events->notification.signal();
    }
    if (auto* notification{ m_events->groupNotification.load() })
        notification->signal();
}

bool StepperMotor::prepareCoordinated(bool forward, std::shared_ptr<const hal::step::Sequence> sequence)
{
    stopAndWait();
    std::scoped_lock lock{ m_mutex };
    m_events->referenceActivated.store(false);
    m_events->referenceReleased.store(false);
    if (!isReferenced() || (forward && referenceSwitchActive()) ||
        !m_stepOutput->prepareSequence(std::move(sequence)))
        return false;
    m_motionSign = forward ? 1.0 : -1.0;
    m_dirOutput->write(forward ? hal::gpio::Level::High : hal::gpio::Level::Low);
    return true;
}

bool StepperMotor::coordinatedBlocked() const noexcept
{
    return m_motionSign > 0.0 &&
      (m_events->referenceActivated.load() || referenceSwitchActive());
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
    if (!timingFor(velocity))
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    auto request{ std::make_shared<VelocityRequest>() };
    request->velocity = velocity;
    auto future{ request->completion.get_future() };
    {
        std::scoped_lock lock{ m_queueMutex };
        if (!m_busy || m_homing || !m_profileActive.load() || m_velocityRequest)
            return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
        m_velocityRequest = request;
        m_events->notification.signal();
    }
    return future.get();
}

void StepperMotor::finishQueue(Result result) noexcept
{
    m_profileActive.store(false);
    m_wakeAtPulse.store(std::numeric_limits<hal::step::PulseCount>::max());
    std::scoped_lock lock{ m_queueMutex };
    if (m_pending) {
        m_pending->completion.set_value(result);
        m_pending.reset();
    }
    if (m_velocityRequest) {
        m_velocityRequest->completion.set_value(
          std::unexpected(std::make_error_code(std::errc::operation_canceled)));
        m_velocityRequest.reset();
    }
    m_busy = m_homing = false;
}

std::future<StepperMotor::Result> StepperMotor::startMotion(std::packaged_task<Result(std::stop_token)> task)
{
    std::scoped_lock lock{ m_workerMutex };
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
    auto future{ task.get_future() };
    {
        std::scoped_lock queue_lock{ m_queueMutex };
        m_busy = m_homing = true;
    }
    try {
        // Homing keeps packaged_task/call_once frames alive through motion and
        // formatted debug logging. The default 4 KiB stack is too small on ARM
        // and corrupts the thread's TLS/control block before cancellation ends.
        m_worker = runtime::thread::create_jthread(
          { .name = "homing", .stack_size = 16384U },
          [this, task = std::move(task)](std::stop_token stop) mutable {
            task(stop);
            finishQueue(Result::Stopped);
            // Publish completion before waking the coordinator, including when
            // homing ended without another STEP or GPIO callback.
            if (auto* notification{ m_events->groupNotification.load() })
                notification->signal();
        });
    } catch (...) {
        finishQueue(Result::Faulted);
        throw;
    }
    return future;
}

StepperMotor::Result StepperMotor::performReference(pnm::units::AngularVelocity seek_velocity,
                                                    pnm::units::AngularVelocity latch_velocity,
                                                    pnm::units::Time timeout,
                                                    std::stop_token stop)
{
    const auto motor_id{ static_cast<unsigned>(m_id) + 1U };
    const auto now{ std::chrono::steady_clock::now() };
    if (!timingFor(seek_velocity) || !timingFor(latch_velocity) || latch_velocity >= seek_velocity ||
        !timeout.isFinite() || timeout <= 0_s ||
        timeout >= (std::chrono::steady_clock::time_point::max() - now) / 2) {
        pnm::log::debug("Motor {} reference rejected: invalid seek/latch speeds or overall timeout",
                        motor_id);
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
        pnm::log::debug("Motor {} reference: seeking first contact Forward at {} rpm",
                        motor_id,
                        seek_velocity.get<pnm::units::AngularVelocityUnits::rpm>());
        const auto first{ seek(Direction::Forward, seek_velocity, High) };
        if (first != Result::Completed) {
            return first;
        }
    }
    else {
        pnm::log::debug("Motor {} reference: switch already active", motor_id);
    }

    pnm::log::debug("Motor {} reference: confirming first contact (10 ms stable HIGH)", motor_id);
    auto result{ waitReferenceLevel(High, deadline, stop) };
    if (result != Result::Completed) {
        return result;
    }

    pnm::log::debug("Motor {} reference: backing off Backward at {} rpm until release",
                    motor_id,
                    seek_velocity.get<pnm::units::AngularVelocityUnits::rpm>());
    result = seek(Direction::Backward, seek_velocity, Low);
    if (result != Result::Completed) {
        return result;
    }

    pnm::log::debug("Motor {} reference: confirming release (10 ms stable LOW)", motor_id);
    result = waitReferenceLevel(Low, deadline, stop);
    if (result != Result::Completed) {
        return result;
    }

    pnm::log::debug("Motor {} reference: seeking second contact Forward at {} rpm",
                    motor_id,
                    latch_velocity.get<pnm::units::AngularVelocityUnits::rpm>());
    result = seek(Direction::Forward, latch_velocity, High);
    if (result != Result::Completed) {
        return result;
    }

    pnm::log::debug("Motor {} reference: confirming second contact (10 ms stable HIGH)", motor_id);
    result = waitReferenceLevel(High, deadline, stop);
    if (result != Result::Completed) {
        return result;
    }

    pnm::log::debug("Motor {} reference: applying switch position {} deg",
                    motor_id,
                    m_referenceSwitchPosition.get<pnm::units::AngleUnits::deg>());
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

    if (!m_feedback->reference(m_referenceSwitchPosition))
        return Result::Faulted;
    if (stop.stop_requested())
        return Result::Stopped;

    m_position.store(m_referenceSwitchPosition);
    m_velocity.store(0_rpm);
    m_referenced.store(true);
    // Do not overwrite a counter-fault callback racing with this commit. INDEX
    // may remain unavailable after homing without invalidating the switch datum.
    if (m_feedbackReferenceLost.load())
        m_referenced.store(false);
    return m_referenced.load() ? Result::Completed : Result::Faulted;
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
