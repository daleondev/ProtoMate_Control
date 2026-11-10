#pragma once
#include "ProfileCurve.hpp"
#include "common/motion/OperatingEnvelope.hpp"
#include <bit>
#include <limits>
#include <utility>

namespace io::motion
{
    // Bounded cubic-phase evaluator on the 100 ns scheduling grid. Q32 coefficients retain sub-nanostep
    // resolution. Ambiguous comparisons use the original double evaluator;
    // fixed-point rounding is never silently interpreted as a crossing.
    class ProfileCrossing
    {
        static constexpr double MAX_ROLLING_SECONDS{ 1e6 }, MAX_DISTANCE{ 1'000'000 };
        static constexpr double ENDPOINT_EPSILON_ULPS{ 32 }, COEFFICIENT_EPSILON_ULPS{ 64 };
        static constexpr unsigned MAX_PHASE_SHIFT{ 30 }, EXACT_DOUBLE_BITS{ 52 }, FRACTION_BITS{ 32 };
        static constexpr unsigned BISECTION_ITERATIONS{ 64 }, NEWTON_ITERATIONS{ 8 };
        static constexpr unsigned COEFFICIENT_COUNT{ 4 }, ROUNDING_ALLOWANCE{ 8 };
        static constexpr unsigned MAX_LINEAR_PERIOD{ 1'000'000 }, INITIAL_INTERVAL{ 100 };
        static constexpr unsigned PERIOD_BITS{ 24 }, SHIFT_BITS{ 7 };
        static constexpr double HALF{ .5 };
        static constexpr double JERK_INTEGRATION_DIVISOR{ 6 };

      public:
        explicit ProfileCrossing(const ::motion::Profile& profile, double offset = 0, bool rolling = false)
          : m_reference{ profile }
          , m_offset{ offset }
        {
            const auto description{ profile.description() };
            if (!std::isfinite(offset) || offset < 0 || offset > 1 || !description.size ||
                !std::isfinite(profile.duration()) || profile.duration() <= 0 ||
                profile.duration() >
                  (rolling ? MAX_ROLLING_SECONDS : ::motion::operating::MAX_PROGRAM_SECONDS) ||
                !std::isfinite(description.distance) || description.distance <= 0 ||
                description.distance > MAX_DISTANCE) {
                m_valid = false;
                return;
            }
            m_offsetQ = std::llround(offset * profile_util::Q);
            m_pulses = unsigned(std::floor(description.distance + offset));
            m_count = description.size;
            m_end = std::uint64_t(std::ceil(profile.duration() * profile_util::TICKS_PER_SECOND));
            m_distance = profile.distance();
            m_endAtRest = description.end_velocity == 0;
            // A zero-velocity endpoint is ill-conditioned for root finding:
            // roundoff a few ulps above an integer otherwise advances its last
            // edge by microseconds. Keep the floor-based count, but place an
            // indistinguishable final crossing exactly at rest. Never add a
            // pulse when the endpoint rounds just below the next integer.
            const auto endpoint{ description.distance + offset };
            const auto endpoint_error{ ENDPOINT_EPSILON_ULPS * std::numeric_limits<double>::epsilon() *
                                       std::max(1., endpoint) };
            m_terminal = description.end_velocity == 0 && m_pulses && endpoint - m_pulses <= endpoint_error
                           ? m_pulses
                           : UINT32_MAX;
            double start{};
            for (unsigned i{}; i < m_count; ++i) {
                const auto& original{ description.phases[i] };
                if (!initializePhase(m_phases[i], original, profile, start, rolling)) {
                    m_valid = false;
                    return;
                }
                start += original.duration;
            }
        }
        ProfileCrossing(const ProfileCrossing& other, const ::motion::Profile& profile)
          : m_reference{ other.m_reference, profile }
          , m_offset{ other.m_offset }
          , m_distance{ other.m_distance }
          , m_endAtRest{ other.m_endAtRest }
          , m_offsetQ{ other.m_offsetQ }
          , m_phases{ other.m_phases }
          , m_count{ other.m_count }
          , m_pulses{ other.m_pulses }
          , m_end{ other.m_end }
          , m_terminal{ other.m_terminal }
          , m_long{ other.m_long }
          , m_valid{ other.m_valid }
        {
        }
        bool valid() const noexcept { return m_valid; }
        struct Cursor
        {
            std::uint64_t previous{}, interval{ INITIAL_INTERVAL };
            unsigned evaluations{}, fallbacks{}, phase{};
        };
        std::uint64_t next(unsigned ordinal) noexcept
        {
            const auto tick{ next(ordinal, m_cursor) };
            if (!tick) {
                m_valid = false;
            }
            return tick;
        }
        std::uint64_t next(unsigned ordinal, Cursor& cursor) const noexcept
        {
            if (!ordinal || ordinal > m_pulses) {
                return 0;
            }
            return atPosition(double(ordinal), cursor);
        }
        std::uint64_t atPosition(double ordinal, Cursor& cursor) const noexcept
        {
            if (!m_valid || !std::isfinite(ordinal) || ordinal <= 0) {
                return 0;
            }
            // A mapped stopping endpoint can differ by a few ulps after
            // inverse-path evaluation and subtraction of its cut position.
            // Keep the independently determined count, and place this crossing
            // at rest just like the integer endpoint case below.
            const auto endpoint{ m_distance + m_offset };
            if (m_endAtRest && std::abs(ordinal - endpoint) <= ENDPOINT_EPSILON_ULPS *
                                                                 std::numeric_limits<double>::epsilon() *
                                                                 std::max(1., endpoint)) {
                cursor.previous = m_end;
                return m_end;
            }
            if (ordinal > endpoint) {
                return 0;
            }
            if (ordinal == 1 && m_offset == 1.) {
                cursor.previous = 1;
                return 1;
            }
            if (ordinal == m_terminal) {
                cursor.previous = m_end;
                return m_end;
            }
            if (m_long) {
                if (const auto tick{ m_reference.cruiseCrossing(ordinal - m_offset) }) {
                    cursor.interval = *tick - cursor.previous;
                    cursor.previous = *tick;
                    return *tick;
                }
            }
            if (m_offset == 0 && ordinal == std::floor(ordinal)) {
                if (const auto linear{ linearPosition(ordinal, cursor) }) {
                    return *linear;
                }
            }
            return solvePosition(ordinal, cursor);
        }
        unsigned evaluations() const noexcept { return m_cursor.evaluations; }
        unsigned fallbacks() const noexcept { return m_cursor.fallbacks; }

      private:
        struct Phase
        {
            std::array<std::int64_t, COEFFICIENT_COUNT> coefficients{};
            std::int64_t error{};
            std::uint64_t origin{}, end{};
            std::uint32_t linear_period : PERIOD_BITS{};
            std::uint32_t shift : SHIFT_BITS{};
            std::uint32_t long_linear : 1 {};
            std::int32_t linear_position{};
        };
        struct Value
        {
            std::int64_t position{};
            unsigned phase{};
        };
        bool initializePhase(Phase& p,
                             const ::motion::Profile::PhaseDescription& original,
                             const ::motion::Profile& profile,
                             double start,
                             bool rolling)
        {
            p.origin = std::uint64_t(std::ceil(start * profile_util::TICKS_PER_SECOND));
            p.end = std::uint64_t(std::floor((start + original.duration) * profile_util::TICKS_PER_SECOND));
            const auto length{ p.end >= p.origin ? p.end - p.origin : 0 };
            p.shift = std::max(1U, unsigned(std::bit_width(length)));
            if (p.shift > MAX_PHASE_SHIFT) {
                if (!rolling || original.acceleration != 0 || original.jerk != 0 || original.velocity <= 0) {
                    return false;
                }
                // Long jog cruises use the same affine reference predicate.
                // Their acceleration/deceleration phases retain fixed-point evaluation.
                p.long_linear = true;
                p.shift = 1;
                m_long = true;
                p.coefficients[0] = std::llround(original.position * profile_util::Q);
                return true;
            }
            const auto seconds{ double(std::uint32_t{ 1 } << p.shift) / profile_util::TICKS_PER_SECOND };
            const auto state{ profile.at(static_cast<double>(p.origin) / profile_util::TICKS_PER_SECOND) };
            const std::array<double, COEFFICIENT_COUNT> coefficients{
                state.position,
                state.velocity * seconds,
                state.acceleration * seconds * seconds * HALF,
                original.jerk * seconds * seconds * seconds / JERK_INTEGRATION_DIVISOR
            };
            double magnitude{};
            for (unsigned c{ 0 }; c < COEFFICIENT_COUNT; ++c) {
                magnitude += std::abs(coefficients[c]);
                const auto scaled{ coefficients[c] * profile_util::Q };
                if (!std::isfinite(scaled) ||
                    std::abs(scaled) > double(std::int64_t{ 1 } << EXACT_DOUBLE_BITS)) {
                    return false;
                }
                p.coefficients[c] = std::llround(scaled);
            }
            // Four rounded coefficients, three truncated Horner products,
            // plus a conservative allowance for double coefficient formation.
            p.error = std::int64_t(std::ceil(
              ROUNDING_ALLOWANCE + (COEFFICIENT_EPSILON_ULPS * std::numeric_limits<double>::epsilon() *
                                    magnitude * profile_util::Q)));
            if (original.acceleration == 0 && original.jerk == 0 && original.velocity > 0) {
                const auto period{ profile_util::TICKS_PER_SECOND / original.velocity };
                const auto origin{ start * profile_util::TICKS_PER_SECOND };
                if (period >= 1 && period <= MAX_LINEAR_PERIOD && period == std::floor(period) &&
                    origin == std::floor(origin) && original.position == std::floor(original.position)) {
                    p.linear_period = std::uint32_t(period);
                    p.linear_position = std::int32_t(original.position);
                }
            }
            return true;
        }
        std::uint64_t initialCandidate(const Cursor& cursor, std::int64_t target) const noexcept
        {
            const auto lo{ cursor.previous };
            const auto hi{ m_end };
            auto candidate{ std::clamp(cursor.previous + cursor.interval, lo, hi) };
            if (!cursor.previous) {
                // Seed a new DMA batch near the target phase rather than at
                // time zero. This is only a guess: the full integer bracket
                // and the exact crossing predicate remain authoritative.
                for (unsigned phase{ 0 }; phase < m_count; ++phase) {
                    const auto& p{ m_phases[phase] };
                    if (target < p.coefficients[0]) {
                        break;
                    }
                    candidate =
                      std::clamp(p.origin + (p.end > p.origin ? (p.end - p.origin) / 2 : 0), lo, hi);
                }
            }
            return candidate;
        }
        std::uint64_t solvePosition(double ordinal, Cursor& cursor) const noexcept
        {
            const auto target{ std::llround(ordinal * profile_util::Q) - m_offsetQ };
            std::uint64_t lo{ cursor.previous };
            std::uint64_t hi{ m_end };
            auto candidate{ initialCandidate(cursor, target) };
            for (unsigned iteration{ 0 }; lo < hi && iteration < BISECTION_ITERATIONS; ++iteration) {
                const auto value{ evaluate(candidate, cursor) };
                const bool reached{ crossed(candidate, target, value, cursor, ordinal) };
                if (reached) {
                    hi = candidate;
                    if (candidate == 0 ||
                        !crossed(candidate - 1, target, evaluate(candidate - 1, cursor), cursor, ordinal)) {
                        lo = candidate;
                        break;
                    }
                }
                else {
                    lo = candidate + 1;
                }
                auto guess{ lo + ((hi - lo) / 2) };
                const auto& phase{ m_phases[value.phase] };
                const auto error{ value.position - target };
                const auto velocity{ derivative(candidate, phase) };
                // Newton is an accelerator only. The integer bracket is the
                // authority; overflow/zero derivatives fall back to bisection.
                if (iteration < NEWTON_ITERATIONS && velocity > 0 && lo < hi &&
                    error > -(INT64_MAX >> phase.shift) && error < (INT64_MAX >> phase.shift)) {
                    const auto correction{ estimateQuotient(error * (std::int64_t{ 1 } << phase.shift),
                                                            velocity) };
                    const auto proposed{ std::int64_t(candidate) - correction };
                    // A sub-tick correction truncates to zero. Clamping it to
                    // the narrowed integer bracket avoids a needless jump to
                    // the middle of a long remaining phase.
                    guess = std::uint64_t(std::clamp(proposed, std::int64_t(lo), std::int64_t(hi - 1)));
                }
                candidate = guess;
            }
            cursor.interval = lo - cursor.previous;
            cursor.previous = lo;
            return lo;
        }
        std::optional<std::uint64_t> linearPosition(double ordinal, Cursor& cursor) const noexcept
        {
            for (unsigned i{ 0 }; i < m_count; ++i) {
                const auto& phase{ m_phases[i] };
                if (!phase.linear_period) {
                    continue;
                }
                const auto tick{ std::int64_t(phase.origin) +
                                 ((std::int64_t(ordinal) - phase.linear_position) * phase.linear_period) };
                if (std::cmp_greater(tick, phase.origin) && std::cmp_less_equal(tick, phase.end)) {
                    // Exact inverse of the same certified integer affine
                    // predicate used below; no root search or rounding.
                    cursor.interval = std::uint64_t(tick) - cursor.previous;
                    cursor.previous = std::uint64_t(tick);
                    cursor.phase = i;
                    return std::uint64_t(tick);
                }
            }
            return std::nullopt;
        }
        static std::int64_t estimateQuotient(std::int64_t numerator, std::int64_t denominator) noexcept
        {
            // Newton only proposes the next point in the certified bracket.
            // Normalize its ratio to the M4's hardware 32-bit divider. Rounding
            // this estimate cannot accept a pulse: crossed() still decides that.
            const auto magnitude{ std::uint64_t(numerator < 0 ? -numerator : numerator) };
            const auto divisor{ std::uint64_t(denominator) };
            const auto bits{ static_cast<unsigned>(
              std::max(std::bit_width(magnitude), std::bit_width(divisor))) };
            const auto shift{ bits > FRACTION_BITS ? bits - FRACTION_BITS : 0 };
            const auto scaled_divisor{ std::uint32_t(divisor >> shift) };
            if (!scaled_divisor) {
                return numerator / denominator;
            }
            const auto quotient{ std::uint32_t(magnitude >> shift) / scaled_divisor };
            return numerator < 0 ? -std::int64_t(quotient) : std::int64_t(quotient);
        }
        static std::int64_t multiply(std::int64_t a, std::uint32_t b) noexcept
        {
            return ((a >> FRACTION_BITS) * std::int64_t(b)) +
                   std::int64_t((std::uint64_t(std::uint32_t(a)) * b) >> FRACTION_BITS);
        }
        Value evaluate(std::uint64_t tick, Cursor& cursor) const noexcept
        {
            ++cursor.evaluations;
            unsigned i{ cursor.phase };
            // Most adjacent pulse evaluations stay in the same cubic phase.
            // Newton may probe backwards, so validate the cached phase in both
            // directions; the selected polynomial is identical to a full scan.
            while (i && tick <= m_phases[i - 1].end) {
                --i;
            }
            while (i + 1 < m_count && tick > m_phases[i].end) {
                ++i;
            }
            cursor.phase = i;
            const auto& p{ m_phases[i] };
            if (p.long_linear) {
                return { .position = std::llround(m_reference.position(tick) * profile_util::Q), .phase = i };
            }
            const auto delta{ std::uint32_t(tick >= p.origin ? tick - p.origin : 0) };
            const auto u{ delta << (FRACTION_BITS - p.shift) };
            const auto& c{ p.coefficients };
            return { .position = c[0] + multiply(c[1] + multiply(c[2] + multiply(c[3], u), u), u),
                     .phase = i };
        }
        static std::int64_t derivative(std::uint64_t tick, const Phase& p) noexcept
        {
            if (p.long_linear) {
                return 0;
            }
            const auto u{ std::uint32_t(tick >= p.origin ? tick - p.origin : 0)
                          << (FRACTION_BITS - p.shift) };
            const auto& c{ p.coefficients };
            return c[1] + multiply((2 * c[2]) + multiply(3 * c[3], u), u);
        }
        bool crossed(std::uint64_t tick,
                     std::int64_t target,
                     Value value,
                     Cursor& cursor,
                     double ordinal) const noexcept
        {
            const auto& p{ m_phases[value.phase] };
            if (!p.long_linear && tick >= p.origin && tick <= p.end) {
                // The reference's affine crossing is exactly integer arithmetic
                // in this case. Avoid a software-double fallback at every edge
                // of a grid-aligned cruise (400/500/800 ticks in our fixture).
                if (p.linear_period && m_offset == 0 && ordinal == std::floor(ordinal)) {
                    return std::int64_t(tick) >=
                           std::int64_t(p.origin) +
                             (((target >> FRACTION_BITS) - p.linear_position) * p.linear_period);
                }
                if (value.position - p.error - 1 >= target) {
                    return true;
                }
                if (value.position + p.error + 1 < target) {
                    return false;
                }
            }
            ++cursor.fallbacks;
            return m_reference.crossed(tick, ordinal - m_offset);
        }
        profile_util::Curve m_reference;
        double m_offset{}, m_distance{};
        bool m_endAtRest{};
        std::int64_t m_offsetQ{};
        std::array<Phase, ::motion::Profile::MAX_PHASES> m_phases{};
        unsigned m_count{}, m_pulses{};
        Cursor m_cursor;
        std::uint64_t m_end{};
        std::uint32_t m_terminal{};
        bool m_long{};
        bool m_valid{ true };
    };
}
