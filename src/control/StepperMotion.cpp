#include "MotionSequence.hpp"
#include "StepperMotor.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

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
