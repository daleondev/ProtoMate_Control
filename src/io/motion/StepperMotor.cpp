#include "StepperMotor.hpp"
#include "Limits.hpp"
#include "Sequence.hpp"

#include "pneumo/logging.hpp"
#include "runtime/thread.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

namespace io::motion
{
    namespace
    {
        constexpr std::size_t MOTION_WORKER_STACK_SIZE{ 16384U };
        // Cleanup must finish even after a driver failure, including during stack unwinding.
        void report_cleanup_failure(const hal::util::Result<>& result, const char* message) noexcept
        {
            if (!result) {
                std::fputs(message, stderr);
            }
        }
    }

    StepperMotor::StepperMotor(hal::core::MotorId id,
                               pnm::units::Angle reference_switch_position,
                               pnm::units::Angle full_step_angle,
                               size_t microsteps,
                               const std::shared_ptr<hal::IStepGenerator>& step_generator,
                               std::shared_ptr<hal::device::IStepperDriver> driver,
                               std::shared_ptr<hal::device::IMotorFeedback> feedback,
                               hal::device::IStepperDriver::FaultCallback shutdown)
      : m_id{ id }
      , m_referenceSwitchPosition{ reference_switch_position }
      , m_fullStepAngle{ full_step_angle }
      , m_microsteps{ microsteps }
      , m_stepOutput{ hal::core::create_stepper_step_output(step_generator, id) }
      , m_dirOutput{ hal::core::create_stepper_direction_output(id) }
      , m_referenceSwitchInput{ hal::core::create_reference_limit_switch(id) }
      , m_feedback{ feedback ? std::move(feedback)
                             : hal::core::create_motor_feedback(
                                 id,
                                 { .full_step_angle = full_step_angle, .microsteps = microsteps }) }
      , m_driver{ std::move(driver) }
    {
        // Pneumo assertions implement failure reporting through a C macro.
        // NOLINTBEGIN(cppcoreguidelines-avoid-do-while,cppcoreguidelines-pro-type-vararg,modernize-use-std-print,readability-simplify-boolean-expr)
        PNM_ASSERT(m_referenceSwitchInput, "Motor %d has no reference switch", static_cast<int>(id) + 1);
        // NOLINTEND(cppcoreguidelines-avoid-do-while,cppcoreguidelines-pro-type-vararg,modernize-use-std-print,readability-simplify-boolean-expr)

        if (!reference_switch_position.isFinite() || !full_step_angle.isFinite() ||
            full_step_angle <= 0_deg || microsteps == 0U) {
            throw std::invalid_argument("invalid reference position, step angle or microstep count");
        }
        m_stepAngle = m_fullStepAngle / static_cast<double>(m_microsteps);

        if (!m_stepOutput || !m_dirOutput || !m_referenceSwitchInput || !m_feedback || !m_driver) {
            throw std::runtime_error("motor board resource is unavailable");
        }

        try {
            m_driver->setFaultCallback([this, shutdown = std::move(shutdown)]() mutable noexcept {
                m_referenced.store(false);
                if (shutdown) {
                    shutdown(); // Central shared-enable shutdown, ISR-safe.
                }
                m_events->notification.signal();
                if (auto* notification{ m_events->group_notification.load() }) {
                    notification->signal();
                }
            });
            m_feedback->setCallback([this](const hal::device::IMotorFeedback::Sample& sample) noexcept {
                accountFeedback(sample);
            });
            if (!m_feedback->start()) {
                throw std::runtime_error("motor feedback could not start");
            }

            if (!m_stepOutput->setProgressCallback(
                  [this](const hal::step::AxisStatus& status) noexcept { accountProgress(status); })) {
                throw std::runtime_error("motor progress subscription unavailable");
            }

            static_assert(std::atomic_bool::is_always_lock_free);
            static_assert(std::atomic_uint32_t::is_always_lock_free);
            m_referenceSwitchInput->setEdgeCallback([events = m_events](hal::gpio::Level level) noexcept {
                if (level == hal::gpio::Level::High) {
                    events->reference_activated.store(true);
                }
                else {
                    events->reference_released.store(true);
                }
                events->reference_changes.fetch_add(1U);
                events->notification.signal();
                if (auto* notification{ events->group_notification.load() }) {
                    notification->signal();
                }
            });

            pnm::log::debug("Motor initialized: {} degrees per microstep",
                            m_stepAngle.get<pnm::units::AngleUnits::deg>());
        } catch (...) {
            m_driver->clearFaultCallback();
            m_feedback->clearCallback();
            report_cleanup_failure(m_feedback->stop(), "Motor feedback stop failed during cleanup\n");
            m_referenceSwitchInput->clearEdgeCallback();
            report_cleanup_failure(m_stepOutput->setProgressCallback(nullptr),
                                   "Motor STEP callback cleanup failed\n");
            throw;
        }
    }

    StepperMotor::~StepperMotor()
    {
        stopAndWait();
        m_driver->clearFaultCallback();
        report_cleanup_failure(m_feedback->stop(), "Motor feedback stop failed during cleanup\n");
        m_feedback->clearCallback();
        m_referenceSwitchInput->clearEdgeCallback();
        report_cleanup_failure(m_stepOutput->setProgressCallback(nullptr),
                               "Motor STEP callback cleanup failed\n");
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

    pnm::Result<> StepperMotor::setMotionDefaults(Defaults defaults)
    {
        if (!defaults.acceleration.isFinite() || defaults.acceleration <= 0_rad_s2 ||
            !defaults.deceleration.isFinite() || defaults.deceleration <= 0_rad_s2 ||
            !defaults.jerk.isFinite() || defaults.jerk < 0_rad_s3) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        std::scoped_lock lock{ m_queueMutex };
        if (m_busy) {
            return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
        }
        m_motionDefaults = defaults;
        return {};
    }

    Defaults StepperMotor::motionDefaults() const
    {
        std::scoped_lock lock{ m_queueMutex };
        return m_motionDefaults;
    }

    std::future<StepperMotor::Result> StepperMotor::submitMove(pnm::units::Angle position,
                                                               pnm::units::AngularVelocity velocity,
                                                               Defaults dynamics,
                                                               BufferMode mode,
                                                               pnm::units::Time timeout,
                                                               bool relative)
    {
        const auto epoch = m_abortEpoch.load();
        const auto rejected = [this] {
            util::Completion<Result> promise;
            m_lastCompletion = promise;
            promise.setValue(Result::Rejected);
            return promise.getFuture();
        };
        const auto now{ std::chrono::steady_clock::now() };
        if (!m_driver->status().ready || !position.isFinite() || !timingFor(velocity) ||
            !timeout.isFinite() || timeout < 0_s ||
            timeout >= (std::chrono::steady_clock::time_point::max() - now) / 2 ||
            !dynamics.acceleration.isFinite() || dynamics.acceleration < 0_rad_s2 ||
            !dynamics.deceleration.isFinite() || dynamics.deceleration < 0_rad_s2 ||
            !dynamics.jerk.isFinite() || dynamics.jerk < 0_rad_s3 || mode < BufferMode::Aborting ||
            mode > BufferMode::BlendingHigh || (!relative && !isReferenced())) {
            return rejected();
        }
        std::scoped_lock worker_lock{ m_workerMutex };
        {
            std::scoped_lock lock{ m_queueMutex };
            if (dynamics.acceleration == 0_rad_s2) {
                dynamics.acceleration = m_motionDefaults.acceleration;
            }
            if (dynamics.deceleration == 0_rad_s2) {
                dynamics.deceleration = m_motionDefaults.deceleration;
            }
            if (dynamics.jerk == 0_rad_s3) {
                dynamics.jerk = m_motionDefaults.jerk;
            }
        }
        auto command{ std::make_shared<Command>(Command{ .position = position,
                                                         .velocity = velocity,
                                                         .dynamics = dynamics,
                                                         .mode = mode,
                                                         .timeout = timeout,
                                                         .relative = relative,
                                                         .completion = {},
                                                         .epoch = epoch }) };
        m_lastCompletion = command->completion;
        auto future{ command->completion.getFuture() };
        if (mode != BufferMode::Aborting) {
            std::scoped_lock lock{ m_queueMutex };
            if (m_busy) {
                if (m_homing || m_pending) {
                    command->completion.setValue(Result::Rejected);
                    return future;
                }
                m_pending = command;
                m_events->notification.signal();
                return future;
            }
        }
        if (m_worker.joinable()) {
            static_cast<void>(m_worker.request_stop());
            m_worker.join();
        }
        {
            std::scoped_lock lock{ m_queueMutex };
            m_busy = true;
            m_homing = false;
        }
        try {
            m_worker = runtime::thread::create_jthread(
              { .name = "motion", .stack_size = MOTION_WORKER_STACK_SIZE },
              [this, command](const std::stop_token& stop) { runCommands(command, stop); });
        } catch (...) {
            command->completion.setException(std::current_exception());
            finishQueue(Result::Faulted);
        }
        return future;
    }

    class StepperMotor::CommandRunner
    {
        using Count = hal::step::PulseCount;
        using Clock = std::chrono::steady_clock;

      public:
        CommandRunner(StepperMotor& motor, std::shared_ptr<Command> request, std::stop_token stop_token)
          : m_motor{ &motor }
          , m_command{ std::move(request) }
          , m_stop{ std::move(stop_token) }
          , m_step{ motor.m_stepAngle.get() }
        {
        }
        void run() noexcept
        {
            m_motor->m_runEpoch = m_command->epoch;
            const std::stop_callback cancellation{ m_stop,
                                                   [this] { m_motor->m_events->notification.signal(); } };
            try {
                while (m_command && runCommand()) {
                }
            } catch (...) {
                if (m_update) {
                    m_update->completion.set_value(
                      std::unexpected(std::make_error_code(std::errc::not_enough_memory)));
                }
                static_cast<void>(m_motor->m_stepOutput->stop());
                complete(Result::Faulted);
            }
            static_cast<void>(m_motor->m_stepOutput->stop());
            m_motor->finishQueue(Result::Stopped);
        }

      private:
        void complete(Result result)
        {
            if (!m_resolved) {
                m_command->completion.setValue(result);
                m_resolved = true;
            }
        }
        std::optional<Count> quantize(pnm::units::Angle distance) const
        {
            const auto pulses{ std::round(std::abs(distance.get() / m_step)) };
            // Retain exact integer representation while planning in double precision.
            if (!distance.isFinite() || !std::isfinite(pulses) ||
                pulses >= static_cast<double>(motion::limits::MAX_PULSES)) {
                return {};
            }
            return static_cast<Count>(pulses);
        }
        static ::motion::Profile::Limits limits(const Command& request)
        {
            return ::motion::Profile::Limits{ .velocity = request.velocity.get(),
                                              .acceleration = request.dynamics.acceleration.get(),
                                              .deceleration = request.dynamics.deceleration.get(),
                                              .jerk = request.dynamics.jerk.get() };
        }
        static Clock::time_point deadlineFor(const Command& request)
        {
            return request.timeout == 0_s ? Clock::time_point::max()
                                          : Clock::now() + request.timeout.toChrono<Clock::duration>();
        }
        std::shared_ptr<Command> takeNext()
        {
            std::scoped_lock lock{ m_motor->m_queueMutex };
            if (m_motor->m_velocityRequest) {
                m_motor->m_velocityRequest->completion.set_value(
                  std::unexpected(std::make_error_code(std::errc::operation_canceled)));
                m_motor->m_velocityRequest.reset();
            }
            auto next{ std::exchange(m_motor->m_pending, {}) };
            if (!next) {
                m_motor->m_busy = false;
            }
            return next;
        }
        double junctionSpeed(const Command& next) const
        {
            switch (next.mode) {
                case BufferMode::BlendingLow:
                    return std::min(m_command->velocity.get(), next.velocity.get());
                case BufferMode::BlendingPrevious:
                    return m_command->velocity.get();
                case BufferMode::BlendingNext:
                    return next.velocity.get();
                case BufferMode::BlendingHigh:
                    return std::max(m_command->velocity.get(), next.velocity.get());
                default:
                    return 0.0;
            }
        }
        std::shared_ptr<motion::Sequence> makeBlend(const Command& next,
                                                    Count next_count,
                                                    Count begin,
                                                    ::motion::Profile::State state,
                                                    double speed) const
        {
            auto constraints{ limits(*m_command) };
            constraints.velocity = std::max(constraints.velocity, speed);
            const auto first{ ::motion::Profile::create(static_cast<double>(m_activeEnd - begin) * m_step,
                                                        constraints,
                                                        state.velocity,
                                                        state.acceleration,
                                                        speed) };
            const auto second{ ::motion::Profile::create(
              static_cast<double>(next_count) * m_step, limits(next), speed) };
            if (!first || !second) {
                return {};
            }
            return std::make_shared<motion::Sequence>(
              m_step,
              motion::Sequence::Part{ .profile = *first, .begin = begin, .end = m_activeEnd },
              motion::Sequence::Part{
                .profile = *second, .begin = m_activeEnd, .end = m_activeEnd + next_count });
        }
        template<class Factory>
        static std::shared_ptr<motion::Sequence> feasibleBlend(double junction, const Factory& make)
        {
            auto replacement{ make(junction) };
            // Short or late commands may not reach the requested junction.
            // Find a feasible lower speed, then refine its upper boundary.
            if (!replacement) {
                double upper{ junction };
                for (unsigned i = 1U; i <= motion::limits::BLEND_SEARCH_STEPS; ++i) {
                    const auto lower{ junction *
                                      (1.0 - static_cast<double>(i) /
                                               static_cast<double>(motion::limits::BLEND_SEARCH_STEPS)) };
                    replacement = make(lower);
                    if (replacement) {
                        auto low{ lower };
                        auto high{ upper };
                        for (unsigned j = 0U; j < motion::limits::BLEND_REFINE_STEPS; ++j) {
                            const auto middle{ (low + high) * 0.5 };
                            if (auto candidate{ make(middle) }) {
                                low = middle;
                                replacement = std::move(candidate);
                            }
                            else {
                                high = middle;
                            }
                        }
                        break;
                    }
                    upper = lower;
                }
            }
            return replacement;
        }
        bool blend(Count begin,
                   ::motion::Profile::State state,
                   std::optional<hal::step::ScheduleCursor> cursor)
        {
            std::shared_ptr<Command> next;
            {
                std::scoped_lock lock{ m_motor->m_queueMutex };
                next = m_motor->m_pending;
            }
            if (m_following || !next ||
                (next->mode == BufferMode::Buffered || next->mode == BufferMode::Aborting) ||
                begin >= m_activeEnd || (!next->relative && !m_motor->isReferenced())) {
                return false;
            }
            const auto delta{ next->relative ? next->position : next->position - m_activeTarget };
            const auto next_count{ quantize(delta) };
            if (!next_count || *next_count == 0U || delta.get() * m_sign <= 0.0 ||
                m_activeEnd >
                  static_cast<Count>(static_cast<double>(motion::limits::MAX_PULSES)) - *next_count) {
                return false;
            }
            const auto junction{ junctionSpeed(*next) };
            const auto make = [&](double speed) {
                return makeBlend(*next, *next_count, begin, state, speed);
            };
            auto replacement{ feasibleBlend(junction, make) };
            if (!replacement) {
                return false;
            }
            if (cursor && !m_motor->m_stepOutput->replaceSequence(*cursor, replacement)) {
                return false;
            }
            m_sequence = std::move(replacement);
            m_following = std::move(next);
            m_motor->m_wakeAtPulse.store(m_activeEnd);
            return true;
        }
        std::optional<Result> arm()
        { // Back-to-back submissions can blend before the first DMA buffer is
            // armed, including short moves that fit completely in those buffers.
            static_cast<void>(blend(0U, {}, std::nullopt));
            m_motor->m_events->notification.clear();
            m_motor->m_events->reference_activated.store(false);
            m_motor->m_events->reference_released.store(false);
            if (!m_motor->m_stepOutput->prepareSequence(m_sequence)) {
                return Result::Rejected;
            }
            {
                std::scoped_lock lock{ m_motor->m_mutex };
                if (!m_motor->m_driver->status().ready) {
                    return Result::Faulted;
                }
                if (m_motor->interrupted(m_stop)) {
                    return Result::Stopped;
                }
                if (m_sign > 0.0 && (m_motor->m_events->reference_activated.load() ||
                                     m_motor->m_referenceSwitchInput->read() == hal::gpio::Level::High)) {
                    return Result::Rejected;
                }
                m_motor->m_dirOutput->write(m_sign > 0.0 ? hal::gpio::Level::High : hal::gpio::Level::Low);
                m_motor->m_motionSign = m_sign;
                m_motor->m_profileActive.store(true);
                const auto delay{ std::chrono::nanoseconds{
                  static_cast<std::int64_t>(std::ceil((m_sequence->timeAtPulse(1U) + 0.01) * 1e9)) } };
                if (!m_motor->m_stepOutput->start(delay)) {
                    return Result::Rejected;
                }
            }

            return std::nullopt;
        }
        void advance(Count pulses)
        {
            if (m_following && pulses >= m_activeEnd) {
                m_motor->m_wakeAtPulse.store(std::numeric_limits<Count>::max());
                const auto previous{ m_command };
                const auto old_end{ m_activeEnd };
                m_activeEnd = m_sequence->count();
                m_activeTarget +=
                  m_motor->m_stepAngle * (m_sign * static_cast<double>(m_activeEnd - old_end));
                {
                    std::scoped_lock lock{ m_motor->m_queueMutex };
                    m_motor->m_pending.reset();
                }
                m_command = std::move(m_following);
                m_resolved = false;
                m_deadline = deadlineFor(*m_command);
                previous->completion.setValue(Result::Completed);
            }
        }
        void updateVelocity()
        {
            {
                std::scoped_lock lock{ m_motor->m_queueMutex };
                m_update = std::exchange(m_motor->m_velocityRequest, {});
            }
            if (m_update) {
                auto update_result{ pnm::Result<Count>{
                  std::unexpected(std::make_error_code(std::errc::operation_not_permitted)) } };
                const auto old_velocity{ m_command->velocity };
                for (unsigned attempt = 0; attempt < 3U; ++attempt) {
                    const auto cursor{ m_motor->m_stepOutput->scheduleCursor() };
                    if (!cursor || cursor->terminal || cursor->first_uncommitted + 1U >= m_activeEnd) {
                        break;
                    }
                    const auto begin{ cursor->first_uncommitted + 1U };
                    const auto state{ m_sequence->atPulse(begin) };
                    m_command->velocity = m_update->velocity;
                    const auto end_speed{ m_following ? m_sequence->first().profile.endVelocity() : 0.0 };
                    const auto profile{ ::motion::Profile::create(static_cast<double>(m_activeEnd - begin) *
                                                                    m_step,
                                                                  limits(*m_command),
                                                                  state.velocity,
                                                                  state.acceleration,
                                                                  end_speed) };
                    if (!profile) {
                        break;
                    }
                    auto replacement{ std::make_shared<motion::Sequence>(
                      m_step,
                      motion::Sequence::Part{ .profile = *profile, .begin = begin, .end = m_activeEnd },
                      m_following ? m_sequence->second() : std::nullopt) };
                    const auto applied{ m_motor->m_stepOutput->replaceSequence(*cursor, replacement) };
                    if (applied) {
                        m_sequence = std::move(replacement);
                        update_result = begin; // 1-based first pulse with a changed following interval.
                        break;
                    }
                    update_result = std::unexpected(applied.error());
                    if (applied.error() != std::errc::resource_unavailable_try_again) {
                        break;
                    }
                }
                if (!update_result) {
                    m_command->velocity = old_velocity;
                }
                m_update->completion.set_value(update_result);
                m_update.reset();
            }
        }
        void blendPending()
        {
            if (!m_following) {
                for (unsigned attempt = 0U; attempt < 3U && !m_following; ++attempt) {
                    const auto cursor{ m_motor->m_stepOutput->scheduleCursor() };
                    if (!cursor || cursor->terminal || cursor->first_uncommitted + 1U >= m_activeEnd) {
                        break;
                    }
                    const auto begin{ cursor->first_uncommitted + 1U };
                    if (blend(begin, m_sequence->atPulse(begin), *cursor)) {
                        break;
                    }
                    const auto after{ m_motor->m_stepOutput->scheduleCursor() };
                    if (!after || after->first_uncommitted == cursor->first_uncommitted) {
                        break;
                    }
                }
            }
        }
        Result wait()
        {
            using enum hal::step::State;
            auto result{ Result::Stopped };
            while (true) {
                const auto status{ m_motor->m_stepOutput->status() };
                if (!m_motor->m_driver->status().ready || !status.counts_exact || status.state == Underrun ||
                    status.state == DmaError) {
                    result = Result::Faulted;
                    break;
                }
                advance(status.pulses);
                if (status.state == Completed) {
                    result = Result::Completed;
                    break;
                }
                if (m_motor->interrupted(m_stop) || status.state == Stopped ||
                    (m_sign > 0.0 && m_motor->m_events->reference_activated.load())) {
                    break;
                }
                if (status.state != Running || (!m_command->relative && !m_motor->isReferenced())) {
                    result = Result::Faulted;
                    break;
                }
                if (Clock::now() >= m_deadline) {
                    result = Result::TimedOut;
                    break;
                }

                updateVelocity();
                blendPending();
                static_cast<void>(m_motor->m_events->notification.waitUntil(m_deadline));
            }
            return result;
        }
        bool runCommand()
        {
            m_resolved = false;
            m_motor->m_wakeAtPulse.store(std::numeric_limits<Count>::max());
            if (!m_motor->m_driver->status().ready) {
                complete(Result::Faulted);
                return false;
            }
            if (m_motor->interrupted(m_stop)) {
                complete(Result::Stopped);
                return false;
            }
            if (!m_command->relative && !m_motor->isReferenced()) {
                complete(Result::Rejected);
                return false;
            }
            const auto origin{ m_motor->setpointPosition() };
            const auto distance{ m_command->relative ? m_command->position : m_command->position - origin };
            const auto count{ quantize(distance) };
            if (!count) {
                complete(Result::Rejected);
                return false;
            }
            if (*count == 0U) {
                complete(Result::Completed);
                m_command = takeNext();
                return true;
            }
            m_sign = distance.get() >= 0.0 ? 1.0 : -1.0;
            m_activeEnd = *count;
            m_activeTarget = origin + m_motor->m_stepAngle * (m_sign * static_cast<double>(*count));
            auto initial{ ::motion::Profile::create(static_cast<double>(*count) * m_step,
                                                    limits(*m_command)) };
            if (!initial) {
                complete(Result::Rejected);
                return false;
            }
            m_sequence = std::make_shared<motion::Sequence>(
              m_step, motion::Sequence::Part{ .profile = *initial, .begin = 0U, .end = *count });
            m_following.reset();
            m_deadline = deadlineFor(*m_command);

            if (const auto terminal{ arm() }) {
                complete(*terminal);
                return false;
            }
            auto result{ wait() };
            const auto final{ m_motor->m_stepOutput->stop() };
            m_motor->m_profileActive.store(false);
            if (!final.counts_exact || final.state == hal::step::State::Underrun ||
                final.state == hal::step::State::DmaError) {
                result = Result::Faulted;
            }
            complete(result);
            if (result != Result::Completed || m_motor->interrupted(m_stop)) {
                return false;
            }
            m_command = takeNext();

            return true;
        }
        StepperMotor* m_motor;
        std::shared_ptr<Command> m_command;
        std::stop_token m_stop;
        double m_step;
        bool m_resolved{};
        std::shared_ptr<VelocityRequest> m_update;
        double m_sign{};
        Count m_activeEnd{};
        pnm::units::Angle m_activeTarget;
        std::shared_ptr<motion::Sequence> m_sequence;
        std::shared_ptr<Command> m_following;
        Clock::time_point m_deadline;
    };

    void StepperMotor::runCommands(std::shared_ptr<Command> command, const std::stop_token& stop) noexcept
    {
        CommandRunner{ *this, std::move(command), stop }.run();
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

        return startMotion([this, seek_velocity, latch_velocity, timeout](const std::stop_token& stop) {
            const auto result{ performReference(seek_velocity, latch_velocity, timeout, stop) };
            pnm::log::debug("Motor {} reference ended: {}, referenced={}, position={} deg",
                            static_cast<unsigned>(m_id) + 1U,
                            result,
                            isReferenced(),
                            m_position.load().get<pnm::units::AngleUnits::deg>());
            return result;
        });
    }

    bool StepperMotor::isReferenced() const noexcept { return m_referenced.load(); }

    bool StepperMotor::referenceSwitchActive() const noexcept
    {
        return m_referenceSwitchInput->read() == hal::gpio::Level::High;
    }

    void StepperMotor::invalidateReference() noexcept
    {
        m_referenced.store(false);
        m_feedback->invalidate();
    }

    pnm::units::Angle StepperMotor::setpointPosition() const
    {
        std::scoped_lock lock{ m_mutex };
        static_cast<void>(m_stepOutput->status());
        return m_position.load();
    }

    pnm::units::AngularVelocity StepperMotor::setpointVelocity() const
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
            if (auto* notification{ m_events->group_notification.load() }) {
                notification->signal();
            }
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

        if (!status.counts_exact) {
            m_feedback->invalidate();
        }
        else {
            m_feedback->motion(status.state == hal::step::State::Running, m_motionSign > 0, status.period);
        }

        auto boundary{ m_wakeAtPulse.load() };
        const bool reached{ status.pulses >= boundary &&
                            m_wakeAtPulse.compare_exchange_strong(
                              boundary, std::numeric_limits<hal::step::PulseCount>::max()) };
        if (status.state != hal::step::State::Running || reached) {
            m_events->notification.signal();
        }
        if (auto* notification{ m_events->group_notification.load() }) {
            notification->signal();
        }
    }

    bool StepperMotor::prepareCoordinated(bool forward, std::shared_ptr<const hal::step::Sequence> sequence)
    {
        stopAndWait();
        std::scoped_lock lock{ m_mutex };
        m_events->reference_activated.store(false);
        m_events->reference_released.store(false);
        if (!m_driver->status().ready || !isReferenced() || (forward && referenceSwitchActive()) ||
            !m_stepOutput->prepareSequence(std::move(sequence))) {
            return false;
        }
        m_motionSign = forward ? 1.0 : -1.0;
        m_dirOutput->write(forward ? hal::gpio::Level::High : hal::gpio::Level::Low);
        return true;
    }

    bool StepperMotor::coordinatedBlocked() const noexcept
    {
        return m_motionSign > 0.0 && (m_events->reference_activated.load() || referenceSwitchActive());
    }

    void StepperMotor::requestStop() noexcept
    {
        m_abortEpoch.fetch_add(1);
        static_cast<void>(m_stepOutput->stop());
        m_events->notification.signal();
    }

    void StepperMotor::stop() noexcept
    {
        requestStop();
        std::scoped_lock worker_lock{ m_workerMutex };
        static_cast<void>(m_worker.request_stop());

        std::scoped_lock state_lock{ m_mutex };
        static_cast<void>(m_stepOutput->stop());
    }

    void StepperMotor::stopAndWait() noexcept
    {
        requestStop();
        std::scoped_lock worker_lock{ m_workerMutex };
        static_cast<void>(m_worker.request_stop());

        {
            std::scoped_lock state_lock{ m_mutex };
            static_cast<void>(m_stepOutput->stop());
        }

        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

    std::optional<hal::step::Timing> StepperMotor::timingFor(
      pnm::units::AngularVelocity velocity) const noexcept
    {
        if (!velocity.isFinite() || velocity <= 0_rpm) {
            return std::nullopt;
        }

        auto time{ m_stepAngle / velocity };
        if (!time.isFinite() || time < pnm::units::Time{ motion::limits::MIN_PERIOD } ||
            time > motion::limits::MAX_PERIOD) {
            return std::nullopt;
        }

        const double seconds{ time.get<pnm::units::TimeUnits::s>() };
        return hal::step::Timing{
            .period = std::chrono::nanoseconds{ static_cast<std::int64_t>(
              std::ceil(seconds * motion::limits::NANOSECONDS_PER_SECOND)) },
            .high_time = 5us,
        };
    }

    pnm::Result<hal::step::PulseCount> StepperMotor::setVelocity(pnm::units::AngularVelocity velocity)
    {
        if (!m_driver->status().ready) {
            return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
        }
        if (!timingFor(velocity)) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        auto request{ std::make_shared<VelocityRequest>() };
        request->velocity = velocity;
        auto future{ request->completion.get_future() };
        {
            std::scoped_lock lock{ m_queueMutex };
            if (!m_busy || m_homing || !m_profileActive.load() || m_velocityRequest) {
                return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
            }
            m_velocityRequest = request;
            m_events->notification.signal();
        }
        return future.get();
    }

    bool StepperMotor::interrupted(const std::stop_token& stop) const noexcept
    {
        return stop.stop_requested() || m_abortEpoch.load() != m_runEpoch;
    }

    void StepperMotor::finishQueue(Result result) noexcept
    {
        m_profileActive.store(false);
        m_wakeAtPulse.store(std::numeric_limits<hal::step::PulseCount>::max());
        std::scoped_lock lock{ m_queueMutex };
        if (m_pending) {
            m_pending->completion.setValue(result);
            m_pending.reset();
        }
        if (m_velocityRequest) {
            m_velocityRequest->completion.set_value(
              std::unexpected(std::make_error_code(std::errc::operation_canceled)));
            m_velocityRequest.reset();
        }
        m_busy = m_homing = false;
    }

    std::future<StepperMotor::Result> StepperMotor::startMotion(std::function<Result(std::stop_token)> task)
    {
        const auto epoch = m_abortEpoch.load();
        std::scoped_lock lock{ m_workerMutex };
        if (m_worker.joinable()) {
            static_cast<void>(m_worker.request_stop());
            m_worker.join();
        }
        util::Completion<Result> completion;
        m_lastCompletion = completion;
        auto future{ completion.getFuture() };
        {
            std::scoped_lock queue_lock{ m_queueMutex };
            m_busy = m_homing = true;
        }
        try {
            // Homing keeps packaged_task/call_once frames alive through motion and
            // formatted debug logging. The default 4 KiB stack is too small on ARM
            // and corrupts the thread's TLS/control block before cancellation ends.
            m_worker = runtime::thread::create_jthread(
              { .name = "homing", .stack_size = MOTION_WORKER_STACK_SIZE },
              [this, task = std::move(task), completion, epoch](const std::stop_token& stop) mutable {
                m_runEpoch = epoch;
                Result result{ Result::Faulted };
                try {
                    result = task(stop);
                } catch (...) {
                    result = Result::Faulted;
                }
                finishQueue(Result::Stopped);
                completion.setValue(result);
                // Publish completion before waking the coordinator, including when
                // homing ended without another STEP or GPIO callback.
                if (auto* notification{ m_events->group_notification.load() }) {
                    notification->signal();
                }
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
                                                        const std::stop_token& stop)
    {
        const auto motor_id{ static_cast<unsigned>(m_id) + 1U };
        const auto now{ std::chrono::steady_clock::now() };
        if (!m_driver->status().ready || !timingFor(seek_velocity) || !timingFor(latch_velocity) ||
            latch_velocity >= seek_velocity || !timeout.isFinite() || timeout <= 0_s ||
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
            explicit FinishReferencing(std::atomic_bool& value)
              : flag(value)
            {
            }
            FinishReferencing(const FinishReferencing&) = delete;
            FinishReferencing& operator=(const FinishReferencing&) = delete;
            FinishReferencing(FinishReferencing&&) = delete;
            FinishReferencing& operator=(FinishReferencing&&) = delete;
            ~FinishReferencing() { flag.store(false); }
        } finish{ m_referencing };

        const auto deadline{ now + timeout.toChrono<std::chrono::steady_clock::duration>() };
        const std::stop_callback cancellation{ stop, [this] { m_events->notification.signal(); } };
        const auto seek =
          [&](Direction direction, pnm::units::AngularVelocity speed, hal::gpio::Level level) {
            if (interrupted(stop)) {
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
                                                          const std::stop_token& stop)
    {
        constexpr auto settle_time{ 10ms };
        std::optional<std::chrono::steady_clock::time_point> stable_since;
        auto changes{ m_events->reference_changes.load() };
        while (true) {
            if (!m_driver->status().ready) {
                return Result::Faulted;
            }
            if (interrupted(stop)) {
                return Result::Stopped;
            }

            const auto now{ std::chrono::steady_clock::now() };
            if (now >= deadline) {
                return Result::TimedOut;
            }

            const auto current_changes{ m_events->reference_changes.load() };
            const auto current_level{ m_referenceSwitchInput->read() };
            if (current_changes != changes || current_level != level) {
                stable_since.reset();
            }

            if (!stable_since && current_level == level) {
                stable_since = now;
            }

            changes = current_changes;
            if (stable_since && now >= *stable_since + settle_time &&
                m_events->reference_changes.load() == current_changes) {
                return Result::Completed;
            }

            static_cast<void>(m_events->notification.waitUntil(
              stable_since ? std::min(deadline, *stable_since + settle_time) : deadline));
        }
    }

    StepperMotor::Result StepperMotor::applyReferencePosition(const std::stop_token& stop)
    {
        std::scoped_lock lock{ m_mutex };
        if (interrupted(stop)) {
            return Result::Stopped;
        }

        const auto status{ m_stepOutput->status() };
        if (!status.counts_exact || status.state == hal::step::State::DmaError ||
            status.state == hal::step::State::Underrun ||
            m_referenceSwitchInput->read() != hal::gpio::Level::High) {
            return Result::Faulted;
        }

        if (!m_driver->status().ready) {
            return Result::Faulted;
        }
        if (!m_feedback->reference(m_referenceSwitchPosition)) {
            return Result::Faulted;
        }
        if (interrupted(stop)) {
            return Result::Stopped;
        }

        m_position.store(m_referenceSwitchPosition);
        m_velocity.store(0_rpm);
        m_referenced.store(true);
        // Do not overwrite a counter-fault callback racing with this commit. INDEX
        // may remain unavailable after homing without invalidating the switch datum.
        if (m_feedbackReferenceLost.load() || !m_driver->status().ready) {
            m_referenced.store(false);
        }
        return m_referenced.load() ? Result::Completed : Result::Faulted;
    }

    StepperMotor::Result StepperMotor::performMotion(Direction direction,
                                                     pnm::units::AngularVelocity velocity,
                                                     pnm::units::Time timeout,
                                                     const std::stop_token& stop,
                                                     std::optional<hal::step::PulseCount> count,
                                                     std::optional<hal::gpio::Level> switch_target)
    {
        if (!m_driver->status().ready) {
            return Result::Rejected;
        }
        const auto timing{ timingFor(velocity) };
        if (!timing || !timeout.isFinite() || timeout < 0_s) {
            pnm::log::warn("Motor move rejected: invalid velocity or timeout");
            return Result::Rejected;
        }

        const auto now{ std::chrono::steady_clock::now() };
        const auto available{ (std::chrono::steady_clock::time_point::max() - now) / 2 };
        if (timeout >= available) {
            pnm::log::warn("Motor move rejected: invalid timeout={} s",
                           timeout.get<pnm::units::TimeUnits::s>());
            return Result::Rejected;
        }

        const auto deadline{ timeout == 0_s ? std::chrono::steady_clock::time_point::max()
                                            : now + timeout.toChrono<std::chrono::steady_clock::duration>() };

        m_events->notification.clear();
        const std::stop_callback cancellation{ stop, [this] { m_events->notification.signal(); } };

        if (interrupted(stop)) {
            return Result::Stopped;
        }
        if (count == 0U) {
            return Result::Completed;
        }

        m_events->reference_activated.store(false);
        m_events->reference_released.store(false);
        const bool toward_reference{ direction == Direction::Forward };

        if (!m_stepOutput->prepare(*timing, count)) {
            pnm::log::warn("Motor move rejected: invalid timing");
            return Result::Rejected;
        }

        {
            std::scoped_lock lock{ m_mutex };
            if (interrupted(stop)) {
                return Result::Stopped;
            }

            if (!m_driver->status().ready) {
                return Result::Faulted;
            }

            if (switch_target && referenceTargetReached(switch_target)) {
                return Result::Completed;
            }

            if (!switch_target && toward_reference &&
                (m_events->reference_activated.load() ||
                 m_referenceSwitchInput->read() == hal::gpio::Level::High)) {
                pnm::log::warn("Motor move rejected: reference limit switch active");
                return Result::Rejected;
            }

            m_dirOutput->write(direction == Direction::Forward ? hal::gpio::Level::High
                                                               : hal::gpio::Level::Low);
            m_motionSign = direction == Direction::Forward ? 1.0 : -1.0;
            if (!m_stepOutput->start()) {
                pnm::log::warn("Motor move rejected: step generation failed");
                return Result::Rejected;
            }
        }

        auto result{ Result::Stopped };
        try {
            result = waitForMotion(toward_reference, switch_target, deadline, stop);
        } catch (...) {
            static_cast<void>(m_stepOutput->stop());
            throw;
        }

        const auto final{ m_stepOutput->stop() };

        if (!final.counts_exact || final.state == hal::step::State::DmaError ||
            final.state == hal::step::State::Underrun) {
            result = Result::Faulted;
        }

        pnm::log::debug("Motor motion ended: {}, position={} deg",
                        result,
                        setpointPosition().get<pnm::units::AngleUnits::deg>());
        return result;
    }

    bool StepperMotor::referenceTargetReached(std::optional<hal::gpio::Level> switch_target) const
    {
        return switch_target &&
               (m_referenceSwitchInput->read() == *switch_target ||
                (*switch_target == hal::gpio::Level::High ? m_events->reference_activated.load()
                                                          : m_events->reference_released.load()));
    }
    StepperMotor::Result StepperMotor::waitForMotion(bool toward_reference,
                                                     std::optional<hal::gpio::Level> switch_target,
                                                     std::chrono::steady_clock::time_point deadline,
                                                     const std::stop_token& stop)
    {
        auto result{ Result::Stopped };
        while (true) {
            if (!switch_target && toward_reference && m_events->reference_activated.load()) {
                break;
            }
            const auto status{ m_stepOutput->status() };

            if (!m_driver->status().ready || status.state == hal::step::State::Underrun ||
                status.state == hal::step::State::DmaError || !status.counts_exact) {
                pnm::log::warn("Motor move faulted: {}", status.state);
                result = Result::Faulted;
                break;
            }
            if (status.state == hal::step::State::Completed) {
                result = Result::Completed;
                break;
            }
            if (interrupted(stop) || status.state == hal::step::State::Stopped) {
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
            if (referenceTargetReached(switch_target)) {
                result = Result::Completed;
                break;
            }

            static_cast<void>(m_events->notification.waitUntil(deadline));
        }
        return result;
    }

}
