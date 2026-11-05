#pragma once
#include "CompactRuns.hpp"
#include <bit>
#include <limits>

namespace motion_bench
{
    // Experimental cubic-phase evaluator. Q32 coefficients retain sub-nanostep
    // resolution. Ambiguous comparisons use the original double evaluator;
    // fixed-point rounding is never silently interpreted as a crossing.
    class FixedCrossing
    {
      public:
        explicit FixedCrossing(const motion::Profile& profile) : m_reference(profile)
        {
            const auto description = profile.description();
            if (!description.size || !std::isfinite(profile.duration()) || profile.duration() <= 0 ||
                profile.duration() > 10 || !std::isfinite(description.distance) ||
                description.distance <= 0 || description.distance > 1'000'000) {
                m_valid = false; return;
            }
            m_pulses = unsigned(std::floor(description.distance));
            m_count = description.size;
            m_end = std::uint32_t(std::ceil(profile.duration() * 1e7));
            m_terminal = description.distance == std::floor(description.distance)
                           ? std::uint32_t(description.distance) : UINT32_MAX;
            double start{};
            for (unsigned i = 0; i < m_count; ++i) {
                auto& p = m_phases[i];
                const auto& original = description.phases[i];
                p.origin = std::uint32_t(std::ceil(start * 1e7));
                p.end = std::uint32_t(std::floor((start + original.duration) * 1e7));
                const auto length = p.end >= p.origin ? p.end - p.origin : 0;
                p.shift = std::max(1U, unsigned(std::bit_width(length)));
                if (p.shift > 30) { m_valid = false; return; }
                const auto seconds = std::ldexp(1., p.shift) / compact::TICKS_PER_SECOND;
                const auto state = profile.at(p.origin / compact::TICKS_PER_SECOND);
                const std::array<double, 4> coefficients{
                    state.position, state.velocity * seconds,
                    state.acceleration * seconds * seconds * .5,
                    original.jerk * seconds * seconds * seconds / 6};
                double magnitude{};
                for (unsigned c = 0; c < 4; ++c) {
                    magnitude += std::abs(coefficients[c]);
                    const auto scaled = coefficients[c] * compact::Q;
                    if (!std::isfinite(scaled) || std::abs(scaled) > double(std::int64_t{1} << 52)) {
                        m_valid = false; return;
                    }
                    p.coefficients[c] = std::llround(scaled);
                }
                // Four rounded coefficients, three truncated Horner products,
                // plus a conservative allowance for double coefficient formation.
                p.error = std::int64_t(std::ceil(8 + 64 * std::numeric_limits<double>::epsilon() * magnitude * compact::Q));
                if (original.acceleration == 0 && original.jerk == 0 && original.velocity > 0) {
                    const auto period = compact::TICKS_PER_SECOND / original.velocity;
                    const auto origin = start * compact::TICKS_PER_SECOND;
                    if (period >= 1 && period <= 1'000'000 && period == std::floor(period) &&
                        origin == std::floor(origin) && original.position == std::floor(original.position)) {
                        p.linearPeriod = std::uint32_t(period);
                        p.linearPosition = std::int32_t(original.position);
                    }
                }
                start += original.duration;
            }
        }
        bool valid() const noexcept { return m_valid; }
        struct Cursor
        {
            std::uint32_t previous{}, interval{100};
            unsigned evaluations{}, fallbacks{};
        };
        std::uint32_t next(unsigned ordinal) noexcept
        {
            const auto tick = next(ordinal, m_cursor);
            if (!tick) m_valid = false;
            evaluations = m_cursor.evaluations;
            fallbacks = m_cursor.fallbacks;
            return tick;
        }
        std::uint32_t next(unsigned ordinal, Cursor& cursor) const noexcept
        {
            if (!m_valid || !ordinal || ordinal > m_pulses) { return 0; }
            if (ordinal == m_terminal) { cursor.previous = m_end; return m_end; }
            const auto target = std::int64_t(ordinal) << 32;
            std::uint32_t lo = cursor.previous, hi = m_end;
            auto candidate = std::clamp(cursor.previous + cursor.interval, lo, hi);
            for (unsigned iteration = 0; lo < hi && iteration < 64; ++iteration) {
                const auto value = evaluate(candidate, cursor);
                const bool reached = crossed(candidate, target, value, cursor);
                if (reached) {
                    hi = candidate;
                    if (candidate == 0 || !crossed(candidate - 1, target, evaluate(candidate - 1, cursor), cursor)) {
                        lo = candidate; break;
                    }
                }
                else lo = candidate + 1;
                auto guess = lo + (hi - lo) / 2;
                const auto& phase = m_phases[value.phase];
                const auto error = value.position - target;
                const auto velocity = derivative(candidate, phase);
                // Newton is an accelerator only. The integer bracket is the
                // authority; overflow/zero derivatives fall back to bisection.
                if (iteration < 8 && velocity > 0 && lo < hi &&
                    error > -(INT64_MAX >> phase.shift) && error < (INT64_MAX >> phase.shift)) {
                    const auto correction = estimateQuotient(error * (std::int64_t{1} << phase.shift), velocity);
                    const auto proposed = std::int64_t(candidate) - correction;
                    // A sub-tick correction truncates to zero. Clamping it to
                    // the narrowed integer bracket avoids a needless jump to
                    // the middle of a long remaining phase.
                    guess = std::uint32_t(std::clamp(proposed, std::int64_t(lo), std::int64_t(hi - 1)));
                }
                candidate = guess;
            }
            cursor.interval = lo - cursor.previous;
            cursor.previous = lo;
            return lo;
        }
        unsigned evaluations{}, fallbacks{};
      private:
        struct Phase
        {
            std::array<std::int64_t, 4> coefficients{};
            std::int64_t error{};
            std::uint32_t origin{}, end{};
            unsigned shift{};
            std::uint32_t linearPeriod{};
            std::int32_t linearPosition{};
        };
        struct Value { std::int64_t position{}; unsigned phase{}; };
        static std::int64_t estimateQuotient(std::int64_t numerator, std::int64_t denominator) noexcept
        {
            // Newton only proposes the next point in the certified bracket.
            // Normalize its ratio to the M4's hardware 32-bit divider. Rounding
            // this estimate cannot accept a pulse: crossed() still decides that.
            const auto magnitude = std::uint64_t(numerator < 0 ? -numerator : numerator);
            const auto divisor = std::uint64_t(denominator);
            const auto bits = std::max(std::bit_width(magnitude), std::bit_width(divisor));
            const auto shift = bits > 32 ? bits - 32 : 0;
            const auto scaledDivisor = std::uint32_t(divisor >> shift);
            if (!scaledDivisor) return numerator / denominator;
            const auto quotient = std::uint32_t(magnitude >> shift) / scaledDivisor;
            return numerator < 0 ? -std::int64_t(quotient) : std::int64_t(quotient);
        }
        static std::int64_t multiply(std::int64_t a, std::uint32_t b) noexcept
        {
            return (a >> 32) * std::int64_t(b) + std::int64_t((std::uint64_t(std::uint32_t(a)) * b) >> 32);
        }
        Value evaluate(std::uint32_t tick, Cursor& cursor) const noexcept
        {
            ++cursor.evaluations;
            unsigned i{};
            while (i + 1 < m_count && tick > m_phases[i].end) ++i;
            const auto& p = m_phases[i];
            const auto delta = tick >= p.origin ? tick - p.origin : 0;
            const auto u = delta << (32 - p.shift);
            const auto& c = p.coefficients;
            return {c[0] + multiply(c[1] + multiply(c[2] + multiply(c[3], u), u), u), i};
        }
        static std::int64_t derivative(std::uint32_t tick, const Phase& p) noexcept
        {
            const auto u = (tick >= p.origin ? tick - p.origin : 0) << (32 - p.shift);
            const auto& c = p.coefficients;
            return c[1] + multiply(2 * c[2] + multiply(3 * c[3], u), u);
        }
        bool crossed(std::uint32_t tick, std::int64_t target, Value value, Cursor& cursor) const noexcept
        {
            const auto& p = m_phases[value.phase];
            if (tick >= p.origin && tick <= p.end) {
                // The reference's affine crossing is exactly integer arithmetic
                // in this case. Avoid a software-double fallback at every edge
                // of a grid-aligned cruise (400/500/800 ticks in our fixture).
                if (p.linearPeriod)
                    return std::int64_t(tick) >= std::int64_t(p.origin) +
                           ((target >> 32) - p.linearPosition) * p.linearPeriod;
                if (value.position - p.error >= target) return true;
                if (value.position + p.error < target) return false;
            }
            ++cursor.fallbacks;
            return m_reference.crossed(tick, double(target) / compact::Q);
        }
        compact::Curve m_reference;
        std::array<Phase, motion::Profile::MAX_PHASES> m_phases{};
        unsigned m_count{}, m_pulses{};
        Cursor m_cursor;
        std::uint32_t m_end{}, m_terminal{};
        bool m_valid{true};
    };
}
