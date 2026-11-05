#include "ProfileSchedule.hpp"

namespace io::motion
{
    namespace
    {
        auto invalid() { return std::unexpected(std::make_error_code(std::errc::invalid_argument)); }
        constexpr double FREQUENCY = 10'000'000.;
    }
    ProfileSchedule::Nominal::Nominal(const ::motion::Profile& value, std::uint32_t half)
      : profile(value), crossing(profile), half_pulses(half)
    {
        if (!crossing.valid())
            return;
        count = unsigned(std::floor(profile.distance()));
        if (!count)
            return;
        ProfileCrossing::Cursor cursor;
        first = crossing.next(1, cursor);
        end = std::uint32_t(std::ceil(profile.duration() * FREQUENCY));
        maximum = std::chrono::nanoseconds(
          std::uint64_t(std::ceil(profile.maximumStepInterval(1) * FREQUENCY) + 4) * 100);
        valid = first != 0;
    }
    ProfileSchedule::ProfileSchedule(const ::motion::Profile& profile)
      : ProfileSchedule(std::make_shared<Nominal>(profile))
    {
    }
    ProfileSchedule::ProfileSchedule(Source nominal)
      : m_source(std::move(nominal))
    {
        if (!m_source || !m_source->valid)
            return;
        m_count = m_source->count;
        m_first = m_source->first;
        m_end = m_source->end;
        m_maximum = m_source->maximum;
        m_valid = true;
    }
    ProfileSchedule::ProfileSchedule(const ProfileSchedule& nominal,
                                     std::uint32_t cut,
                                     ::motion::Profile::Limits limits)
      : ProfileSchedule(nominal.m_source, cut, limits)
    {
        if (!nominal.valid() || nominal.m_brake)
            m_valid = false;
    }
    ProfileSchedule::ProfileSchedule(Source source,
                                     std::uint32_t cut,
                                     ::motion::Profile::Limits limits)
      : m_source(std::move(source)), m_cut(cut)
    {
        if (!m_source || !m_source->valid || cut >= m_source->end)
            return;
        const auto& nominal = *m_source;
        const auto& original = nominal.profile;
        const auto state = original.at(cut / FREQUENCY);
        // If the certified original trajectory is already braking all the way
        // to rest, retain that exact tail. Re-solving its final ramp can turn
        // a zero endpoint velocity into a tiny negative rounding residual.
        double phaseStart{};
        bool alreadyBraking = state.acceleration < 0;
        const auto description = original.description();
        for (unsigned i = 0; i < description.size; ++i) {
            const auto& p = description.phases[i];
            if (phaseStart + p.duration > cut / FREQUENCY &&
                (p.acceleration > 1e-8 || p.acceleration + p.jerk * p.duration > 1e-8))
                alreadyBraking = false;
            phaseStart += p.duration;
        }
        if (alreadyBraking) {
            m_count = nominal.count;
            m_first = nominal.first;
            m_end = nominal.end;
            m_maximum = nominal.maximum;
            m_valid = true;
            return;
        }
        m_prefix = unsigned(std::floor(state.position));
        // The integer-tick crossing predicate, not a rounded position, owns
        // the boundary. Correct at most the adjacent integer crossing.
        ProfileCrossing::Cursor cursor;
        if (m_prefix && m_source->crossing.next(m_prefix, cursor) > cut)
            --m_prefix;
        cursor = {};
        if (m_prefix < nominal.count && m_source->crossing.next(m_prefix + 1, cursor) <= cut)
            ++m_prefix;
        const double rawFraction = state.position - m_prefix;
        if (rawFraction < -1e-9 || rawFraction > 1 + 1e-9)
            return;
        const double fraction = std::clamp(rawFraction, 0., 1.);
        m_brake = ::motion::Profile::brake(state.velocity, state.acceleration, limits);
        if (!m_brake || m_brake->duration() <= 0 || cut / FREQUENCY + m_brake->duration() > 10)
            return;
        m_braking.emplace(*m_brake, fraction);
        if (!m_braking->valid())
            return;
        m_count = m_prefix + unsigned(std::floor(m_brake->distance() + fraction));
        m_first = nominal.first;
        m_end = cut + std::uint32_t(std::ceil(m_brake->duration() * FREQUENCY));
        m_maximum = nominal.maximum + std::chrono::nanoseconds(std::uint64_t(m_end - cut) * 100);
        m_valid = true;
    }
    std::optional<std::int64_t> ProfileSchedule::commonPrefix(
      const hal::step::Sequence& previous) const noexcept
    {
        const auto* other = dynamic_cast<const ProfileSchedule*>(&previous);
        if (!other || !m_valid || !other->m_valid || !(m_source->profile == other->m_source->profile) || m_first != other->m_first)
            return {};
        return std::int64_t(
                 std::min(m_braking ? m_cut : m_end, other->m_braking ? other->m_cut : other->m_end)) -
               m_first;
    }
    std::uint32_t ProfileSchedule::initialOffset() const noexcept
    {
        if (!m_count || !m_braking || m_prefix)
            return 0;
        return risingTick(0).value_or(0);
    }
    std::chrono::nanoseconds ProfileSchedule::maximumPeriod() const noexcept
    {
        return std::max(m_maximum, std::chrono::nanoseconds(40'000));
    }
    std::uint32_t ProfileSchedule::absoluteTick(unsigned pulse,
                                                ProfileCrossing::Cursor& nominal,
                                                ProfileCrossing::Cursor& brake) const noexcept
    {
        if (m_braking && pulse >= m_prefix)
            return m_cut + m_braking->next(pulse - m_prefix + 1, brake);
        return m_source->crossing.next(pulse + 1, nominal);
    }
    hal::util::Result<std::uint32_t> ProfileSchedule::risingTick(hal::step::PulseCount pulse) const noexcept
    {
        if (!m_valid || pulse >= m_count)
            return invalid();
        if (!pulse && (!m_braking || m_prefix))
            return 0;
        ProfileCrossing::Cursor a, b;
        const auto tick = absoluteTick(unsigned(pulse), a, b);
        if (tick < m_first)
            return invalid();
        return tick - m_first;
    }
    hal::util::Result<hal::step::Timing> ProfileSchedule::timing(hal::step::PulseCount pulse) const noexcept
    {
        hal::step::Timing value;
        if (const auto result = generate(pulse, std::span(&value, 1)); !result)
            return std::unexpected(result.error());
        return value;
    }
    hal::util::Result<> ProfileSchedule::generate(hal::step::PulseCount first,
                                                  std::span<hal::step::Timing> target) const noexcept
    {
        if (!m_valid || first > m_count || target.size() > m_count - first)
            return invalid();
        if (target.empty())
            return {};
        ProfileCrossing::Cursor nominal, brake;
        auto previous = absoluteTick(unsigned(first), nominal, brake);
        for (auto& entry : target) {
            ++first;
            const auto next =
              first < m_count ? absoluteTick(unsigned(first), nominal, brake) : previous + 400;
            if (next < previous || next - previous < 400)
                return invalid();
            entry = { std::chrono::nanoseconds(std::uint64_t(next - previous) * 100),
                      std::chrono::microseconds(5) };
            previous = next;
        }
        return {};
    }
}
