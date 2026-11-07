#include "Executor.hpp"
#include "hal/drivers/util/StepHardware.hpp"
#include "hal/drivers/util/StepTrace.hpp"
#include "profiling/Trace.hpp"
#include <stdexcept>

extern "C" {
unsigned motion_debug_executor_abort{};
}
namespace io::motion
{
    namespace
    {
        auto fail(std::errc e) { return std::unexpected(std::make_error_code(e)); }
        bool valid(const ::motion::Profile& profile, ::motion::Profile::Limits limits)
        {
            if (!std::isfinite(limits.velocity) || limits.velocity <= 0 || limits.velocity > 25'000 ||
                !std::isfinite(limits.acceleration) || limits.acceleration <= 0 ||
                !std::isfinite(limits.deceleration) || limits.deceleration <= 0 ||
                !std::isfinite(limits.jerk) || limits.jerk <= 0) {
                return false;
            }
            const auto start = profile.at(0);
            const auto end = profile.at(profile.duration());
            if (start.position != 0 || start.velocity != 0 || start.acceleration != 0 ||
                profile.endVelocity() != 0 || std::abs(end.acceleration) > 1e-8 ||
                profile.distance() != std::floor(profile.distance())) {
                return false;
            }
            const auto d = profile.description();
            double previousAcceleration{};
            for (unsigned i = 0; i < d.size; ++i) {
                const auto& p = d.phases[i];
                if (std::abs(p.acceleration - previousAcceleration) >
                    1e-10 * std::max({ 1., limits.acceleration, limits.deceleration })) {
                    return false;
                }
                previousAcceleration = p.acceleration + (p.jerk * p.duration);
                const auto check = [&](double t) {
                    const auto a = p.acceleration + (p.jerk * t);
                    const auto v = p.velocity + (t * (p.acceleration + p.jerk * t / 2));
                    return a <= limits.acceleration * (1 + 1e-10) &&
                           -a <= limits.deceleration * (1 + 1e-10) && v >= -1e-8 &&
                           v <= limits.velocity * (1 + 1e-10);
                };
                if (std::abs(p.jerk) > limits.jerk * (1 + 1e-10) || !check(0) || !check(p.duration) ||
                    (p.jerk && !check(std::clamp(-p.acceleration / p.jerk, 0., p.duration)))) {
                    return false;
                }
            }
            return true;
        }
    }
    Executor::Executor(std::shared_ptr<hal::IStepGenerator> generator,
                       Prepare prepare,
                       std::pmr::memory_resource* memory)
      : m_generator(std::move(generator))
      , m_prepare(std::move(prepare))
      , m_memory(memory)
    {
        if (!m_generator || !m_prepare || !m_memory) {
            throw std::invalid_argument("executor hardware required");
        }
        for (unsigned i = 0; i < 3; ++i) {
            m_outputs[i] = m_generator->output(hal::step::Axis(i));
        }
    }
    Executor::~Executor()
    {
        if (reserved()) {
            abort();
        }
    }
    auto Executor::sequences(unsigned bank) const
      -> std::array<std::shared_ptr<const hal::step::TimedSequence>, 3>
    {
        std::array<std::shared_ptr<const hal::step::TimedSequence>, 3> result;
        for (unsigned i = 0; i < 3; ++i) {
            if (m_nominal[i] && !m_completed[i])
                result[i] = { m_banks[bank][i], &*m_banks[bank][i]->schedule };
        }
        return result;
    }
    bool Executor::build(unsigned bank, std::uint64_t cut)
    {
        std::optional<::motion::Profile> sharedBrake;
        if (m_progress && cut < std::ceil(m_progress->duration() * 1e7) &&
            !m_progress->brakingToRest(cut / 1e7)) {
            const auto state = m_progress->at(cut / 1e7);
            sharedBrake = ::motion::Profile::brake(state.velocity, state.acceleration, m_progressLimits);
            if (!sharedBrake) {
                return failed(ScheduleFailure::BrakeState, cut);
            }
            if (state.position + sharedBrake->distance() > m_progress->distance() + 1e-9) {
                return failed(ScheduleFailure::BrakeOutsidePath, cut, 3, sharedBrake->distance());
            }
        }
        for (unsigned i = 0; i < 3; ++i) {
            if (!m_nominal[i] || m_completed[i]) {
                continue;
            }
            auto& storage = m_banks[bank][i];
            if (storage.use_count() != 1) {
                return failed(ScheduleFailure::BankInUse, cut, i);
            }
            auto& slot = storage->schedule;
            if (cut >= m_nominal[i]->end) {
                slot.emplace(m_nominal[i]);
            }
            else {
                const auto scaledBrake =
                  sharedBrake ? sharedBrake->scaled(m_nominal[i]->profile.distance() / m_progress->distance())
                              : std::optional<::motion::Profile>{};
                if (sharedBrake && !scaledBrake) {
                    return failed(ScheduleFailure::BrakeScaling, cut, i);
                }
                slot.emplace(m_nominal[i], cut, m_limits[i], scaledBrake ? &*scaledBrake : nullptr);
            }
            if (!slot->valid()) {
                return failed(slot->failure(), cut, i);
            }
        }
        return true;
    }
    hal::util::Result<> Executor::submit(const Command& command)
    {
        const auto result = prepare(command);
        return result ? start(command.id) : result;
    }
    hal::util::Result<> Executor::prepare(const Command& command, bool bind_outputs)
    {
        profiling::Scope measurement{ PROFILE_SITE_MOTION_PREPARE, PROFILE_CAT_MOTION };
        if (reserved()) {
            return fail(std::errc::device_or_resource_busy);
        }
        if (!command.id || command.id <= m_lastId) {
            return fail(std::errc::invalid_argument);
        }
        if (m_status.state == State::Faulted) {
            return fail(std::errc::state_not_recoverable);
        }
        const auto hw = m_generator->status();
        if (hw.state != hal::step::State::Running || !hw.counts_exact) {
            return fail(std::errc::device_or_resource_busy);
        }
        std::uint8_t mask{};
        for (unsigned i = 0; i < 3; ++i) {
            if (command.axes[i].profile) {
                if (hw.axes[i] == hal::step::State::Running) {
                    return fail(std::errc::device_or_resource_busy);
                }
                mask |= 1U << i;
            }
        }
        releaseSchedules();
        STEP_TRACE(calibrate());
        m_status = { .id = command.id };
        m_mask = mask;
        m_rolling = command.rolling;
        struct Rollback
        {
            Executor& owner;
            bool committed{};
            ~Rollback()
            {
                if (!committed) {
                    owner.releaseSchedules();
                }
            }
        } rollback{ .owner = *this };
        // Only these outputs belong to this group. Other axes may be active.
        for (unsigned i = 0; i < 3; ++i) {
            if (!(m_mask & (1U << i))) {
                continue;
            }
            m_forward[i] = command.axes[i].forward;
            for (auto& bank : m_banks) {
                bank[i] = std::allocate_shared<Slot>(std::pmr::polymorphic_allocator<Slot>{ m_memory });
            }
        }
        if (!prepareProfiles(command)) {
            return fail(std::errc::invalid_argument);
        }
        m_completed = {};
        for (unsigned i = 0; i < 3; ++i) {
            m_limits[i] = command.axes[i].limits;
        }
        m_bank = 0;
        if (!build(0, HORIZON)) {
            return fail(std::errc::invalid_argument);
        }
        m_status = { .state = State::Prepared, .id = command.id, .stopping_at = HORIZON };
        if (bind_outputs) {
            const auto bound = bind();
            if (!bound) {
                return bound;
            }
        }
        m_lastId = command.id;
        rollback.committed = true;
        return {};
    }
    hal::util::Result<> Executor::bind()
    {
        if (m_status.state != State::Prepared || m_outputsBound) {
            return fail(std::errc::operation_not_permitted);
        }
        const auto tails = sequences(0);
        m_outputsBound = true;
        for (unsigned i = 0; i < 3; ++i) {
            if (tails[i] && (!m_outputs[i]->clear() || !m_prepare(i, m_forward[i], tails[i]))) {
                releaseSchedules();
                m_status.state = State::Idle;
                return fail(std::errc::operation_not_permitted);
            }
        }
        return {};
    }
    bool Executor::prepareProfiles(const Command& command)
    {
        m_progress.reset();
        if (command.progress) {
            m_progress = ::motion::Profile::fromDescription(*command.progress);
            if (!m_progress || m_progress->distance() <= 0 || !valid(*m_progress, command.progress_limits)) {
                return false;
            }
            m_progressLimits = command.progress_limits;
        }
        bool selected{};
        for (unsigned i = 0; i < 3; ++i) {
            m_nominal[i].reset();
            if (!command.axes[i].profile) {
                continue;
            }
            const auto p = ::motion::Profile::fromDescription(*command.axes[i].profile);
            if (!p || !valid(*p, command.axes[i].limits)) {
                return false;
            }
            if (m_progress) {
                const auto expected = m_progress->scaled(p->distance() / m_progress->distance());
                if (!expected || expected->description().size != p->description().size) {
                    return false;
                }
                const auto a = expected->description();
                const auto b = p->description();
                for (unsigned n = 0; n < a.size; ++n) {
                    const auto& x = a.phases[n];
                    const auto& y = b.phases[n];
                    if (x.duration != y.duration || std::abs(x.position - y.position) > 1e-8 ||
                        std::abs(x.velocity - y.velocity) > 1e-8 ||
                        std::abs(x.acceleration - y.acceleration) > 1e-8 ||
                        std::abs(x.jerk - y.jerk) > 1e-8) {
                        return false;
                    }
                }
            }
            const auto& path = command.axes[i].path;
            if (path.size) {
                if (!m_progress || !path.valid(p->distance()))
                    return false;
                const auto bounds = path.dynamicBounds(command.progress_limits);
                const auto limits = command.axes[i].limits;
                if (!std::ranges::all_of(bounds, [](double v) { return std::isfinite(v) && v >= 0; }) ||
                    bounds[0] > limits.velocity * (1 + 1e-8) ||
                    bounds[1] > std::min(limits.acceleration, limits.deceleration) * (1 + 1e-8) ||
                    bounds[2] > limits.jerk * (1 + 1e-8))
                    return false;
            }
            m_nominal[i] = std::allocate_shared<ProfileSchedule::Nominal>(
              std::pmr::polymorphic_allocator<ProfileSchedule::Nominal>{ m_memory },
              *p,
              DMA_HALF_PULSES,
              m_rolling,
              command.axes[i].path);
            if (!m_nominal[i]->valid) {
                return false;
            }
            selected = true;
        }
        return selected;
    }
    hal::util::Result<> Executor::start(std::uint64_t id)
    {
        if (m_status.state != State::Prepared || id != m_status.id || !m_outputsBound) {
            return fail(std::errc::operation_not_permitted);
        }
        const auto tails = sequences(0);
        std::array<std::optional<std::chrono::nanoseconds>, 3> delays;
        for (unsigned i = 0; i < 3; ++i) {
            if (tails[i])
                delays[i] = std::chrono::milliseconds(5) +
                            std::chrono::nanoseconds(std::uint64_t(m_nominal[i]->first) * 100);
        }
        if (const auto r = m_generator->startPrepared(delays); !r) {
            abort();
            return r;
        }
        const auto time = m_generator->timeline();
        if (!time) {
            abort();
            return fail(std::errc::io_error);
        }
        for (unsigned i = 0; i < 3; ++i) {
            if (tails[i]) {
                m_origin =
                  hal::util::step_add(time->first_rise[i], hal::util::STEP_PARK - m_nominal[i]->first);
                break;
            }
        }
        m_status.state = State::Running;
        m_clockStarted = false;
        m_elapsed = 0;
        m_lastTick = m_origin;
        m_next = RENEW_PERIOD;
        m_lease = LEASE;
        m_final = true;
        for (const auto& p : m_nominal) {
            if (p && p->end > HORIZON)
                m_final = false;
        }
        return {};
    }
    std::optional<std::uint64_t> Executor::elapsed(std::uint32_t tick) noexcept
    {
        if (!m_clockStarted) {
            const auto distance = hal::util::step_distance(m_origin, tick);
            if (distance >= hal::util::STEP_HORIZON) {
                return {};
            }
            m_elapsed = distance;
            m_clockStarted = true;
        }
        else {
            m_elapsed += hal::util::step_distance(m_lastTick, tick);
        }
        m_lastTick = tick;
        return m_elapsed;
    }
    void Executor::brake(Reason reason) noexcept
    {
        m_status.state = State::Braking;
        m_status.reason = reason;
    }
    bool Executor::renew(std::uint64_t id)
    {
        profiling::Scope measurement{ PROFILE_SITE_MOTION_RENEW, PROFILE_CAT_MOTION };
        profile_event(
          PROFILE_CAT_MOTION, PROFILE_MOTION_JOB, std::uint32_t(id), std::uint32_t(id >> 32), 1, 0);
        if (id != m_status.id || m_status.state != State::Running) {
            return false;
        }
        const auto time = m_generator->timeline();
        if (!time) {
            abort();
            return false;
        }
        const auto current = elapsed(time->tick);
        if (!current) {
            m_lease = LEASE;
            return true;
        }
        const auto elapsed = *current;
        if (elapsed >= m_lease) {
            brake(Reason::LeaseExpired);
            return false;
        }
        // A late keepalive cannot revive a schedule which is already braking.
        if (!m_final && elapsed + 20'000 >= m_status.stopping_at) {
            m_status.last_error = int(std::errc::timed_out);
            brake(Reason::RenewalLate);
            return false;
        }
        m_lease = elapsed + LEASE;
        return true;
    }
    bool Executor::stop(std::uint64_t id)
    {
        if (id != m_status.id) {
            return false;
        }
        if (m_status.state == State::Prepared) {
            m_status.state = State::Stopped;
            m_status.reason = Reason::Cancelled;
            releaseSchedules();
            return true;
        }
        if (m_status.state != State::Running) {
            return false;
        }
        const auto time = m_generator->timeline();
        if (time && !elapsed(time->tick)) {
            // Cancellation before the common epoch need not run a brake.
            for (unsigned i = 0; i < 3; ++i) {
                if (m_nominal[i])
                    static_cast<void>(m_outputs[i]->stop());
            }
            m_status.state = State::Stopped;
            m_status.reason = Reason::Cancelled;
            releaseSchedules();
            return true;
        }
        brake(Reason::Cancelled);
        return true;
    }
    void Executor::abort(unsigned site) noexcept
    {
        motion_debug_executor_abort = site;
        for (unsigned i = 0; i < 3; ++i) {
            if (m_outputsBound && (m_mask & (1U << i)))
                static_cast<void>(m_outputs[i]->stop());
        }
        const auto hw = m_generator->status();
        for (unsigned i = 0; i < 3; ++i) {
            m_status.pulses[i] = m_mask & (1U << i) ? hw.pulses[i] : 0;
        }
        m_status.counts_exact = hw.counts_exact;
        m_status.state = State::Faulted;
        m_status.reason = Reason::Hardware;
        releaseSchedules();
    }
    void Executor::releaseSchedules() noexcept
    {
        // Thread context, after every selected output has stopped. Keeping only
        // the status and worker lets independent motor jobs reuse this memory.
        for (unsigned i = 0; i < 3; ++i) {
            if (m_outputsBound && (m_mask & (1U << i)))
                static_cast<void>(m_outputs[i]->clear());
        }
        m_banks = {};
        m_nominal = {};
        m_progress.reset();
        m_mask = 0;
        m_outputsBound = false;
    }
    bool Executor::recover()
    {
        if (reserved()) {
            return false;
        }
        const auto hw = m_generator->status();
        if (hw.state != hal::step::State::Running || !hw.counts_exact) {
            return false;
        }
        m_status = {};
        return true;
    }
    void Executor::service()
    {
        m_generator->preparePending();
        if (!active()) {
            return;
        }
        const auto hw = m_generator->status();
        for (unsigned i = 0; i < 3; ++i) {
            m_status.pulses[i] = m_nominal[i] ? hw.pulses[i] : 0;
        }
        m_status.counts_exact = hw.counts_exact;
        if (hw.state != hal::step::State::Running || !hw.counts_exact) {
            abort();
            return;
        }
        const auto time = m_generator->timeline();
        if (!time) {
            abort();
            return;
        }
        const auto current = elapsed(time->tick);
        // The start epoch is 5 ms ahead of admission; no renewal is due yet.
        if (!current) {
            return;
        }
        const auto elapsed = *current;
        m_status.elapsed = elapsed;
        bool complete = true;
        for (unsigned i = 0; i < 3; ++i) {
            if (!m_nominal[i] || m_completed[i]) {
                continue;
            }
            if (hw.axes[i] == hal::step::State::Completed) {
                // Release this axis's bank ownership without changing its
                // final pulse count. Longer axes can then reuse both banks.
                const auto cleared = m_outputs[i]->clear();
                if (!cleared) {
                    // Completion can still be inside the HAL notification pass.
                    // Retain ownership and immutable storage until callback
                    // teardown permits release; no output is rearmed here.
                    if (cleared.error() == std::errc::device_or_resource_busy) {
                        return;
                    }
                    m_status.last_error = cleared.error().value();
                    abort();
                    return;
                }
                m_completed[i] = true;
            }
            else {
                complete = false;
            }
        }
        if (complete) {
            if (!m_final && m_status.reason == Reason::None) {
                m_status.reason = Reason::RenewalLate;
            }
            m_status.state = m_final && m_status.reason == Reason::None ? State::Completed : State::Stopped;
            releaseSchedules();
            return;
        }
        if (m_status.state == State::Braking) {
            return;
        }
        if (elapsed >= m_lease) {
            brake(Reason::LeaseExpired);
            return;
        }
        if (m_final || elapsed < m_next) {
            return;
        }
        if (elapsed + 20'000 >= m_status.stopping_at) {
            m_status.last_error = int(std::errc::timed_out);
            brake(Reason::RenewalLate);
            return;
        }
        const auto nextCut = elapsed + HORIZON;
        const auto bank = 1U - m_bank;
        const auto building = std::chrono::steady_clock::now();
        STEP_TRACE(
          begin(m_status.id,
                m_origin,
                hal::util::step_add(m_origin, std::uint32_t(m_status.stopping_at % hal::util::STEP_PARK)),
                std::uint32_t(nextCut % hal::util::STEP_PARK)));
        if (!build(bank, nextCut)) {
            STEP_TRACE(finish(m_status.last_error));
            brake(Reason::ScheduleRejected);
            return;
        }
        const auto prepared = std::chrono::steady_clock::now();
        STEP_TRACE(mark(hal::util::step_trace::Built));
        m_status.maximum_build_us = std::max(
          m_status.maximum_build_us,
          std::uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(prepared - building).count()));
        auto replacement = sequences(bank);
        for (unsigned i = 0; i < 3; ++i) {
            if (m_completed[i])
                replacement[i].reset();
        }
        const auto r = m_generator->splice(
          replacement,
          hal::util::step_add(m_origin, std::uint32_t(m_status.stopping_at % hal::util::STEP_PARK)));
        STEP_TRACE(finish(r ? 0 : r.error().value()));
        m_status.maximum_splice_us =
          std::max(m_status.maximum_splice_us,
                   std::uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - prepared)
                                   .count()));
        if (!r) {
            m_status.last_error = r.error().value();
            ++m_status.rejected_renewals;
            if (r.error() == std::errc::resource_unavailable_try_again) {
                return;
            }
            brake(Reason::SpliceRejected);
            return;
        }
        m_bank = bank;
        m_status.stopping_at = nextCut;
        ++m_status.renewals;
        m_next = elapsed + RENEW_PERIOD;
        m_final = true;
        for (const auto& p : m_nominal) {
            if (p && nextCut < p->end)
                m_final = false;
        }
    }
}
