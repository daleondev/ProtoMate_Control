#pragma once
#include "common/motion/Profile.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace io::motion::profile_util
{
    inline constexpr double TICKS_PER_SECOND = 10'000'000.;
    inline constexpr double Q = 4294967296.;
    class Curve
    {
      public:
        explicit Curve(const ::motion::Profile& profile)
          : m_profile(profile)
          , m_phases(profile.description())
        {
            for (unsigned i = 1; i < m_phases.size; ++i) {
                m_starts[i] = m_starts[i - 1] + m_phases.phases[i - 1].duration;
            }
            for (unsigned i = 0; i < m_phases.size; ++i) {
                const auto& p = m_phases.phases[i];
                m_cruise |= p.acceleration == 0 && p.jerk == 0 && p.velocity > 0;
                if (p.acceleration == 0 && p.jerk == 0 && p.velocity > 0) {
                    m_ticksPerStep[i] = TICKS_PER_SECOND / p.velocity;
                }
                m_halfAcceleration[i] = p.acceleration * .5;
                m_sixthJerk[i] = p.jerk / 6;
            }
        }
        Curve(const Curve& other, const ::motion::Profile& profile)
          : m_profile(profile)
          , m_phases(other.m_phases)
          , m_starts(other.m_starts)
          , m_halfAcceleration(other.m_halfAcceleration)
          , m_sixthJerk(other.m_sixthJerk)
          , m_ticksPerStep(other.m_ticksPerStep)
          , m_cruise(other.m_cruise)
        {
        }
        double time(double position) const noexcept { return m_profile.timeAt(position); }
        std::optional<std::uint64_t> cruiseCrossing(double target) const noexcept
        {
            for (unsigned i = 0; i < m_phases.size; ++i) {
                if (i + 1 < m_phases.size && target > m_phases.phases[i + 1].position) {
                    continue;
                }
                const auto& p = m_phases.phases[i];
                if (p.acceleration == 0 && p.jerk == 0 && p.velocity > 0) {
                    return std::uint64_t(std::ceil((m_starts[i] * TICKS_PER_SECOND) +
                                                   ((target - p.position) * m_ticksPerStep[i])));
                }
                break;
            }
            return {};
        }
        bool crossed(std::uint64_t ticks, double target) const noexcept
        {
            // At a zero-velocity endpoint, rounded position evaluation can
            // equal the terminal value several ticks too early. The analytic
            // endpoint time is authoritative (as in Profile::timeAt).
            if (target >= m_phases.distance) {
                return ticks >= std::ceil(m_profile.duration() * TICKS_PER_SECOND);
            }
            for (unsigned i = 0; m_cruise && i < m_phases.size; ++i) {
                if (i + 1 < m_phases.size && target > m_phases.phases[i + 1].position) {
                    continue;
                }
                const auto& p = m_phases.phases[i];
                if (p.acceleration == 0 && p.jerk == 0 && p.velocity > 0) {
                    return ticks >= std::ceil((m_starts[i] * TICKS_PER_SECOND) +
                                              ((target - p.position) * m_ticksPerStep[i]));
                }
                break;
            }
            return position(ticks) >= target;
        }
        double position(std::uint64_t ticks) const noexcept
        {
            // Use the timer's exact integer divisor. Multiplying by the rounded
            // reciprocal can move an exact phase boundary one ULP to its left.
            const double time = ticks / TICKS_PER_SECOND;
            for (unsigned i = 0; i < m_phases.size; ++i) {
                const auto& p = m_phases.phases[i];
                if (time <= m_starts[i] + p.duration) {
                    const auto t = std::clamp(time - m_starts[i], 0., p.duration);
                    return p.position + (t * (p.velocity + t * (m_halfAcceleration[i] + t * m_sixthJerk[i])));
                }
            }
            return m_phases.distance;
        }
        unsigned count() const noexcept { return unsigned(std::floor(m_phases.distance)); }

      private:
        const ::motion::Profile& m_profile;
        ::motion::Profile::Description m_phases;
        std::array<double, ::motion::Profile::MAX_PHASES> m_starts{};
        std::array<double, ::motion::Profile::MAX_PHASES> m_halfAcceleration{}, m_sixthJerk{};
        // Identical double division/rounding to the crossing predicate, done
        // once per immutable profile instead of once per ambiguous STEP edge.
        std::array<double, ::motion::Profile::MAX_PHASES> m_ticksPerStep{};
        bool m_cruise{};
    };

}
