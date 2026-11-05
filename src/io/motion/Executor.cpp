#include "Executor.hpp"
#include "hal/drivers/util/StepHardware.hpp"
#include <stdexcept>

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
                !std::isfinite(limits.jerk) || limits.jerk <= 0)
                return false;
            const auto start = profile.at(0), end = profile.at(profile.duration());
            if (start.position != 0 || start.velocity != 0 || start.acceleration != 0 ||
                profile.endVelocity() != 0 || std::abs(end.acceleration) > 1e-8 ||
                profile.distance() != std::floor(profile.distance()))
                return false;
            const auto d = profile.description();
            double previousAcceleration{};
            for (unsigned i = 0; i < d.size; ++i) {
                const auto& p = d.phases[i];
                if (std::abs(p.acceleration - previousAcceleration) >
                    1e-10 * std::max({ 1., limits.acceleration, limits.deceleration }))
                    return false;
                previousAcceleration = p.acceleration + p.jerk * p.duration;
                const auto check = [&](double t) {
                    const auto a = p.acceleration + p.jerk * t;
                    const auto v = p.velocity + t * (p.acceleration + p.jerk * t / 2);
                    return a <= limits.acceleration * (1 + 1e-10) &&
                           -a <= limits.deceleration * (1 + 1e-10) && v >= -1e-8 &&
                           v <= limits.velocity * (1 + 1e-10);
                };
                if (std::abs(p.jerk) > limits.jerk * (1 + 1e-10) || !check(0) || !check(p.duration) ||
                    (p.jerk && !check(std::clamp(-p.acceleration / p.jerk, 0., p.duration))))
                    return false;
            }
            return true;
        }
    }
    Executor::Executor(std::shared_ptr<hal::IStepGenerator> generator, Prepare prepare,
                       std::pmr::memory_resource* memory)
      : m_generator(std::move(generator))
      , m_prepare(std::move(prepare))
      , m_memory(memory)
    {
        if (!m_generator || !m_prepare || !m_memory)
            throw std::invalid_argument("executor hardware required");
        for (unsigned i = 0; i < 3; ++i)
            m_outputs[i] = m_generator->output(hal::step::Axis(i));
    }
    Executor::~Executor()
    {
        if (reserved())
            abort();
    }
    auto Executor::sequences(unsigned bank) const
      -> std::array<std::shared_ptr<const hal::step::TimedSequence>, 3>
    {
        std::array<std::shared_ptr<const hal::step::TimedSequence>, 3> result;
        for (unsigned i = 0; i < 3; ++i)
            if (m_nominal[i] && !m_completed[i])
                result[i] = { m_banks[bank], &*m_banks[bank]->schedules[i] };
        return result;
    }
    bool Executor::build(unsigned bank, std::uint32_t cut)
    {
        if (m_banks[bank].use_count() != 1) {
            m_status.last_error = int(std::errc::device_or_resource_busy);
            return false;
        }
        for (unsigned i = 0; i < 3; ++i) {
            auto& slot = m_banks[bank]->schedules[i];
            if (!m_nominal[i] || m_completed[i]) {
                slot.reset();
                continue;
            }
            if (cut >= m_nominal[i]->end)
                slot.emplace(m_nominal[i]);
            else
                slot.emplace(m_nominal[i], cut, m_limits[i]);
            if (!slot->valid()) {
                m_status.last_error = int(std::errc::invalid_argument);
                return false;
            }
        }
        return true;
    }
    hal::util::Result<> Executor::submit(const Command& command)
    {
        const auto result = prepare(command);
        return result ? start(command.id) : result;
    }
    hal::util::Result<> Executor::prepare(const Command& command)
    {
        if (reserved())
            return fail(std::errc::device_or_resource_busy);
        if (!command.id || command.id <= m_lastId)
            return fail(std::errc::invalid_argument);
        if (m_status.state == State::Faulted)
            return fail(std::errc::state_not_recoverable);
        const auto hw = m_generator->status();
        if (hw.state != hal::step::State::Running || !hw.counts_exact ||
            std::ranges::any_of(hw.axes, [](auto s) { return s == hal::step::State::Running; }))
            return fail(std::errc::device_or_resource_busy);
        struct Rollback
        {
            Executor& owner;
            bool committed{};
            ~Rollback() { if (!committed) owner.releaseSchedules(); }
        } rollback{ *this };
        // Clear previous ownership before reusing immutable storage. No output
        // is running here, and only this owner may prepare these channels.
        for (unsigned i = 0; i < 3; ++i) {
            if (!m_outputs[i]->clear())
                return fail(std::errc::device_or_resource_busy);
        }
        if (std::ranges::any_of(m_banks, [](const auto& bank) { return bank && bank.use_count() != 1; }))
            return fail(std::errc::device_or_resource_busy);
        for (auto& bank : m_banks) {
            if (!bank)
                bank = std::allocate_shared<Bank>(std::pmr::polymorphic_allocator<Bank>{m_memory});
            for (auto& schedule : bank->schedules)
                schedule.reset();
        }
        for (auto& nominal : m_nominal)
            nominal.reset();
        bool selected{};
        for (unsigned i = 0; i < 3; ++i) {
            m_nominal[i].reset();
            if (!command.axes[i].profile)
                continue;
            const auto p = ::motion::Profile::fromDescription(*command.axes[i].profile);
            if (!p || !valid(*p, command.axes[i].limits))
                return fail(std::errc::invalid_argument);
            m_nominal[i] = std::allocate_shared<ProfileSchedule::Nominal>(
              std::pmr::polymorphic_allocator<ProfileSchedule::Nominal>{m_memory}, *p, DMA_HALF_PULSES);
            if (!m_nominal[i]->valid)
                return fail(std::errc::invalid_argument);
            selected = true;
        }
        if (!selected)
            return fail(std::errc::invalid_argument);
        m_completed = {};
        for (unsigned i = 0; i < 3; ++i)
            m_limits[i] = command.axes[i].limits;
        m_bank = 0;
        if (!build(0, HORIZON))
            return fail(std::errc::invalid_argument);
        const auto tails = sequences(0);
        for (unsigned i = 0; i < 3; ++i)
            if (tails[i]) {
                if (!m_prepare(i, command.axes[i].forward, tails[i]))
                    return fail(std::errc::operation_not_permitted);
            }
        m_status = { .state = State::Prepared, .id = command.id, .stopping_at = HORIZON };
        m_lastId = command.id;
        rollback.committed = true;
        return {};
    }
    hal::util::Result<> Executor::start(std::uint64_t id)
    {
        if (m_status.state != State::Prepared || id != m_status.id)
            return fail(std::errc::operation_not_permitted);
        const auto tails = sequences(0);
        std::array<std::optional<std::chrono::nanoseconds>, 3> delays;
        for (unsigned i = 0; i < 3; ++i)
            if (tails[i])
                delays[i] = std::chrono::milliseconds(5) +
                            std::chrono::nanoseconds(std::uint64_t(m_nominal[i]->first) * 100);
        if (const auto r = m_generator->startPrepared(delays); !r) {
            abort();
            return r;
        }
        const auto time = m_generator->timeline();
        if (!time) {
            abort();
            return fail(std::errc::io_error);
        }
        for (unsigned i = 0; i < 3; ++i)
            if (tails[i]) {
                m_origin =
                  hal::util::step_add(time->first_rise[i], hal::util::STEP_PARK - m_nominal[i]->first);
                break;
            }
        m_status.state = State::Running;
        m_next = RENEW_PERIOD;
        m_lease = LEASE;
        m_final = true;
        for (const auto& p : m_nominal)
            if (p && p->end > HORIZON)
                m_final = false;
        return {};
    }
    void Executor::brake(Reason reason) noexcept
    {
        m_status.state = State::Braking;
        m_status.reason = reason;
    }
    bool Executor::renew(std::uint64_t id)
    {
        if (id != m_status.id || m_status.state != State::Running)
            return false;
        const auto time = m_generator->timeline();
        if (!time) {
            abort();
            return false;
        }
        const auto elapsed = hal::util::step_distance(m_origin, time->tick);
        if (elapsed >= hal::util::STEP_HORIZON) {
            m_lease = LEASE;
            return true;
        }
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
        if (id != m_status.id)
            return false;
        if (m_status.state == State::Prepared) {
            m_status.state = State::Stopped;
            m_status.reason = Reason::Cancelled;
            releaseSchedules();
            return true;
        }
        if (m_status.state != State::Running)
            return false;
        const auto time = m_generator->timeline();
        if (time && hal::util::step_distance(m_origin, time->tick) >= hal::util::STEP_HORIZON) {
            // Cancellation before the common epoch need not run a brake.
            for (unsigned i = 0; i < 3; ++i)
                if (m_nominal[i])
                    static_cast<void>(m_outputs[i]->stop());
            m_status.state = State::Stopped;
            m_status.reason = Reason::Cancelled;
            releaseSchedules();
            return true;
        }
        brake(Reason::Cancelled);
        return true;
    }
    void Executor::abort() noexcept
    {
        const auto hw = m_generator->stop();
        for (unsigned i = 0; i < 3; ++i)
            m_status.pulses[i] = m_nominal[i] || m_completed[i] ? hw.pulses[i] : 0;
        m_status.counts_exact = hw.counts_exact;
        m_status.state = State::Faulted;
        m_status.reason = Reason::Hardware;
        releaseSchedules();
    }
    void Executor::releaseSchedules() noexcept
    {
        // Thread context, after every selected output has stopped. Keeping only
        // the status and worker lets independent motor jobs reuse this memory.
        for (auto& output : m_outputs)
            static_cast<void>(output->clear());
        m_banks = {};
        m_nominal = {};
    }
    bool Executor::recover()
    {
        if (reserved())
            return false;
        const auto hw = m_generator->status();
        if (hw.state != hal::step::State::Running || !hw.counts_exact ||
            std::ranges::any_of(hw.axes, [](auto state) { return state == hal::step::State::Running; }))
            return false;
        m_status = {};
        return true;
    }
    void Executor::service()
    {
        if (!active())
            return;
        const auto hw = m_generator->status();
        for (unsigned i = 0; i < 3; ++i)
            m_status.pulses[i] = m_nominal[i] ? hw.pulses[i] : 0;
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
        const auto elapsed = hal::util::step_distance(m_origin, time->tick);
        // The start epoch is 5 ms ahead of admission; no renewal is due yet.
        if (elapsed >= hal::util::STEP_HORIZON)
            return;
        m_status.elapsed = elapsed;
        bool complete = true;
        for (unsigned i = 0; i < 3; ++i) {
            if (!m_nominal[i] || m_completed[i])
                continue;
            if (hw.axes[i] == hal::step::State::Completed) {
                // Release this axis's bank ownership without changing its
                // final pulse count. Longer axes can then reuse both banks.
                if (!m_outputs[i]->clear()) {
                    abort();
                    return;
                }
                m_completed[i] = true;
            }
            else
                complete = false;
        }
        if (complete) {
            if (!m_final && m_status.reason == Reason::None)
                m_status.reason = Reason::RenewalLate;
            m_status.state = m_final && m_status.reason == Reason::None ? State::Completed : State::Stopped;
            releaseSchedules();
            return;
        }
        if (m_status.state == State::Braking)
            return;
        if (elapsed >= m_lease) {
            brake(Reason::LeaseExpired);
            return;
        }
        if (m_final || elapsed < m_next)
            return;
        if (elapsed + 20'000 >= m_status.stopping_at) {
            m_status.last_error = int(std::errc::timed_out);
            brake(Reason::RenewalLate);
            return;
        }
        const auto nextCut = elapsed + HORIZON;
        const auto bank = 1U - m_bank;
        const auto building = std::chrono::steady_clock::now();
        if (!build(bank, nextCut)) {
            brake(Reason::RenewalLate);
            return;
        }
        const auto prepared = std::chrono::steady_clock::now();
        m_status.maximum_build_us = std::max(
          m_status.maximum_build_us,
          std::uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(prepared - building).count()));
        auto replacement = sequences(bank);
        for (unsigned i = 0; i < 3; ++i)
            if (m_completed[i])
                replacement[i].reset();
        const auto r = m_generator->splice(replacement, hal::util::step_add(m_origin, m_status.stopping_at));
        m_status.maximum_splice_us =
          std::max(m_status.maximum_splice_us,
                   std::uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - prepared)
                                   .count()));
        if (!r) {
            m_status.last_error = r.error().value();
            ++m_status.rejected_renewals;
            if (r.error() == std::errc::resource_unavailable_try_again)
                return;
            brake(Reason::RenewalLate);
            return;
        }
        m_bank = bank;
        m_status.stopping_at = nextCut;
        ++m_status.renewals;
        m_next = elapsed + RENEW_PERIOD;
        m_final = true;
        for (const auto& p : m_nominal)
            if (p && nextCut < p->end)
                m_final = false;
    }
}
