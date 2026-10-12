#include "Profile.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace motion
{
    namespace
    {
        constexpr double HALF = 0.5;
        constexpr double QUADRATIC_FACTOR = 2.0;
        constexpr double CUBIC_INTEGRATION_FACTOR = 6.0;
        constexpr double ACCELERATION_TOLERANCE = 1e-10;
        constexpr double FEASIBILITY_TOLERANCE = 1e-12;
        constexpr double POSITION_TOLERANCE = 1e-9;
        constexpr double MINIMUM_DISTANCE_SCALE = 1e-9;
        constexpr double TIME_TOLERANCE = 1e-12;
        constexpr double MINIMUM_VELOCITY_SCALE = 1e-20;
        constexpr unsigned VELOCITY_SEARCH_ITERATIONS = 64;
        constexpr unsigned TIME_SEARCH_ITERATIONS = 48;

        Profile::State advance(Profile::State s, double t, double jerk) noexcept
        {
            return { .position =
                       s.position +
                       (t * (s.velocity + t * (HALF * s.acceleration + t * jerk / CUBIC_INTEGRATION_FACTOR))),
                     .velocity = s.velocity + (t * (s.acceleration + HALF * t * jerk)),
                     .acceleration = s.acceleration + (t * jerk) };
        }
        double change_duration(double delta, double acceleration, double jerk) noexcept
        {
            delta = std::abs(delta);
            if (jerk == 0.0) {
                return delta / acceleration;
            }
            const auto ramp{ std::min(acceleration / jerk, std::sqrt(delta / jerk)) };
            return ramp + (delta / (jerk * ramp));
        }
        double change_distance(double from, double to, double acceleration, double jerk) noexcept
        {
            if (from == to) {
                return 0.0;
            }
            return HALF * (from + to) * change_duration(to - from, acceleration, jerk);
        }
    }

    void Profile::append(PhaseChange change) noexcept
    {
        const auto [duration, jerk, acceleration] = change;
        if (duration <= 0.0) {
            return;
        }
        m_state.acceleration = acceleration;
        const auto next{ advance(m_state, duration, jerk) };
        m_phases[m_size++] = { .start = m_duration,
                               .duration = duration,
                               .jerk = jerk,
                               .end_position = next.position,
                               .state = m_state };
        m_state = next;
        m_duration += duration;
    }

    void Profile::changeVelocity(VelocityChange change) noexcept
    {
        const auto [target, acceleration, jerk] = change;
        const auto difference{ target - m_state.velocity };
        if (difference == 0.0) {
            return;
        }
        const auto sign{ difference > 0.0 ? 1.0 : -1.0 };
        const auto delta{ std::abs(difference) };
        if (jerk == 0.0) {
            append({ .duration = delta / acceleration, .jerk = 0.0, .acceleration = sign * acceleration });
        }
        else {
            const auto ramp{ std::min(acceleration / jerk, std::sqrt(delta / jerk)) };
            const auto peak{ jerk * ramp };
            append({ .duration = ramp, .jerk = sign * jerk, .acceleration = 0.0 });
            append(
              { .duration = std::max(0.0, (delta / peak) - ramp), .jerk = 0.0, .acceleration = sign * peak });
            append({ .duration = ramp, .jerk = -sign * jerk, .acceleration = sign * peak });
        }
        m_state.velocity = target;
        m_state.acceleration = 0.0;
    }

    bool Profile::normalizeAcceleration(Limits limits) noexcept
    {
        // A live splice retains acceleration as well as velocity. Bring that
        // acceleration continuously to zero before the usual seven phases.
        if (m_state.acceleration != 0.0 && limits.jerk > 0.0) {
            if (m_state.acceleration > limits.acceleration * (1.0 + ACCELERATION_TOLERANCE) ||
                -m_state.acceleration > limits.deceleration * (1.0 + ACCELERATION_TOLERANCE)) {
                return false;
            }
            append({ .duration = std::abs(m_state.acceleration) / limits.jerk,
                     .jerk = -std::copysign(limits.jerk, m_state.acceleration),
                     .acceleration = m_state.acceleration });
            if (m_state.velocity < 0.0 || m_state.position >= m_distance) {
                return false;
            }
        }
        m_state.acceleration = 0.0;
        return true;
    }

    Profile::Preparation Profile::normalizeVelocity(Limits limits) noexcept
    {
        if (m_state.velocity > std::max(limits.velocity, m_endVelocity)) {
            const auto available{ m_distance - m_state.position };
            const auto inherited{ m_state.velocity };
            const auto ceiling{ std::max(limits.velocity, m_endVelocity) };
            const auto via = [&](double cruise) {
                return change_distance(inherited, cruise, limits.deceleration, limits.jerk) +
                       change_distance(cruise, m_endVelocity, limits.deceleration, limits.jerk);
            };
            if (via(ceiling) > available) {
                // A short successor may need one continuous deceleration. Do
                // not insist on flattening acceleration at its cruise speed:
                // that would reject an otherwise feasible high-speed blend.
                if (via(m_endVelocity) > available * (1.0 + FEASIBILITY_TOLERANCE)) {
                    return Preparation::Invalid;
                }
                auto low{ m_endVelocity };
                auto high{ ceiling };
                for (unsigned i = 0; i < VELOCITY_SEARCH_ITERATIONS; ++i) {
                    const auto middle{ (low + high) * HALF };
                    if (via(middle) <= available) {
                        low = middle;
                    }
                    else {
                        high = middle;
                    }
                }
                changeVelocity({ .target = low, .acceleration = limits.deceleration, .jerk = limits.jerk });
                const auto remaining{ std::max(
                  0.0,
                  m_distance - m_state.position -
                    change_distance(low, m_endVelocity, limits.deceleration, limits.jerk)) };
                if (low > 0.0) {
                    append({ .duration = remaining / low, .jerk = 0.0, .acceleration = 0.0 });
                }
                changeVelocity(
                  { .target = m_endVelocity, .acceleration = limits.deceleration, .jerk = limits.jerk });
                return validEndpoint() ? Preparation::Complete : Preparation::Invalid;
            }
            changeVelocity({ .target = ceiling, .acceleration = limits.deceleration, .jerk = limits.jerk });
            if (m_state.position >= m_distance) {
                return Preparation::Invalid;
            }
        }
        return Preparation::Continue;
    }

    bool Profile::appendCruise(Limits limits) noexcept
    {
        const auto from{ m_state.velocity };
        const auto remaining{ m_distance - m_state.position };
        const auto minimum_peak{ std::max(from, m_endVelocity) };
        // A blend can explicitly request a junction faster than the previous
        // command's cruise speed. Preserve an inherited higher velocity too.
        const auto maximum_peak{ std::max(limits.velocity, minimum_peak) };
        const auto required = [&](double peak) {
            return change_distance(from, peak, limits.acceleration, limits.jerk) +
                   change_distance(peak, m_endVelocity, limits.deceleration, limits.jerk);
        };
        const auto minimum_distance{ required(minimum_peak) };
        if (!std::isfinite(minimum_distance) ||
            minimum_distance > remaining * (1.0 + FEASIBILITY_TOLERANCE)) {
            return false;
        }
        auto low{ minimum_peak };
        auto high{ maximum_peak };
        for (unsigned i = 0; i < VELOCITY_SEARCH_ITERATIONS; ++i) {
            const auto middle{ low + ((high - low) * HALF) };
            if (required(middle) > remaining) {
                high = middle;
            }
            else {
                low = middle;
            }
        }
        const auto peak{ required(maximum_peak) <= remaining ? maximum_peak : low };
        if (peak <= 0.0) {
            return false;
        }
        const auto cruise{ std::max(0.0, remaining - required(peak)) / peak };
        changeVelocity({ .target = peak, .acceleration = limits.acceleration, .jerk = limits.jerk });
        append({ .duration = cruise, .jerk = 0.0, .acceleration = 0.0 });
        changeVelocity({ .target = m_endVelocity, .acceleration = limits.deceleration, .jerk = limits.jerk });
        return true;
    }

    bool Profile::validEndpoint() const noexcept
    {
        return std::isfinite(m_duration) && std::isfinite(m_state.position) && m_duration > 0.0 &&
               std::abs(m_state.position - m_distance) <=
                 POSITION_TOLERANCE * std::max(m_distance, MINIMUM_DISTANCE_SCALE);
    }

    std::optional<Profile> Profile::create(double distance,
                                           Limits limits,
                                           double start_velocity,
                                           double start_acceleration,
                                           double end_velocity) noexcept
    {
        if (!std::isfinite(distance) || distance <= 0.0 || !std::isfinite(limits.velocity) ||
            limits.velocity <= 0.0 || !std::isfinite(limits.acceleration) || limits.acceleration <= 0.0 ||
            !std::isfinite(limits.deceleration) || limits.deceleration <= 0.0 ||
            !std::isfinite(limits.jerk) || limits.jerk < 0.0 || !std::isfinite(start_velocity) ||
            start_velocity < 0.0 || !std::isfinite(end_velocity) || end_velocity < 0.0 ||
            !std::isfinite(start_acceleration)) {
            return std::nullopt;
        }

        Profile result;
        result.m_state = { .position = 0.0, .velocity = start_velocity, .acceleration = start_acceleration };
        result.m_distance = distance;
        result.m_endVelocity = end_velocity;
        if (!result.normalizeAcceleration(limits)) {
            return std::nullopt;
        }
        switch (result.normalizeVelocity(limits)) {
            case Preparation::Invalid:
                return std::nullopt;
            case Preparation::Complete:
                return result;
            case Preparation::Continue:
                break;
        }
        if (!result.appendCruise(limits)) {
            return std::nullopt;
        }
        if (!result.validEndpoint()) {
            return std::nullopt;
        }
        return result;
    }

    Profile::State Profile::at(double time) const noexcept
    {
        if (time >= m_duration) {
            return { .position = m_distance, .velocity = m_endVelocity, .acceleration = 0.0 };
        }
        for (unsigned i = 0; i < m_size; ++i) {
            const auto& phase{ m_phases[i] };
            if (time <= phase.start + phase.duration) {
                return advance(phase.state, std::clamp(time - phase.start, 0.0, phase.duration), phase.jerk);
            }
        }
        return { .position = m_distance, .velocity = m_endVelocity, .acceleration = 0.0 };
    }

    double Profile::phaseTimeAt(const Phase& p, double position, TimeHint hint) noexcept
    {
        const auto end = p.end_position;
        const auto distance{ std::max(0.0, position - p.state.position) };
        if (p.jerk == 0.0) {
            const auto denominator{
                p.state.velocity + std::sqrt(std::max(0.0,
                                                      p.state.velocity * p.state.velocity +
                                                        QUADRATIC_FACTOR * p.state.acceleration * distance))
            };
            return p.start + std::clamp(denominator > 0.0 ? QUADRATIC_FACTOR * distance / denominator : 0.0,
                                        0.0,
                                        p.duration);
        }
        if (p.state.velocity == 0.0 && p.state.acceleration == 0.0 && p.jerk > 0.0) {
            return p.start +
                   std::clamp(std::cbrt(CUBIC_INTEGRATION_FACTOR * distance / p.jerk), 0.0, p.duration);
        }
        // Safeguarded Newton inversion of a monotone cubic; fixed bound and
        // no allocation, suitable for the hardware scheduler's refill path.
        double lo{};
        double hi{ p.duration };
        double t{ p.duration * distance / (end - p.state.position) };
        if (hint.seconds > p.start && hint.seconds < p.start + p.duration) {
            const auto local{ hint.seconds - p.start };
            const auto prior{ advance(p.state, local, p.jerk) };
            if (prior.velocity > 0.0 && prior.position <= position) {
                t = std::clamp(local + ((position - prior.position) / prior.velocity), lo, hi);
            }
        }
        for (unsigned iteration = 0; iteration < TIME_SEARCH_ITERATIONS; ++iteration) {
            const auto state{ advance(p.state, t, p.jerk) };
            const auto error{ state.position - position };
            // A picosecond residual is far below the 100 ns timer tick.
            // Avoid bisecting after floating-point position has converged.
            if (std::abs(error) <= std::max(state.velocity, MINIMUM_VELOCITY_SCALE) * TIME_TOLERANCE) {
                break;
            }
            if (error > 0.0) {
                hi = t;
            }
            else {
                lo = t;
            }
            if (hi - lo < TIME_TOLERANCE) {
                break;
            }
            const auto next{ state.velocity > 0.0 ? t - (error / state.velocity) : -1.0 };
            if (next == t) {
                break;
            }
            t = next > lo && next < hi ? next : lo + ((hi - lo) * HALF);
        }
        return p.start + t;
    }

    double Profile::timeAt(double position, TimeHint hint) const noexcept
    {
        if (position <= 0.0) {
            return 0.0;
        }
        if (position >= m_distance) {
            return m_duration;
        }
        for (unsigned i = 0; i < m_size; ++i) {
            const auto& p{ m_phases[i] };
            const auto end{ p.end_position };
            if (position > end && i + 1U < m_size) {
                continue;
            }
            return phaseTimeAt(p, position, hint);
        }
        return m_duration;
    }

    double Profile::maximumStepInterval(double step) const noexcept
    {
        double result{};
        const auto examine = [&](double position) {
            result = std::max(
              result, timeAt(std::min(m_distance, position + step)) - timeAt(std::max(0.0, position - step)));
        };
        examine(0.0);
        examine(m_distance);
        for (unsigned i = 0; i < m_size; ++i) {
            examine(m_phases[i].state.position);
        }
        return result;
    }
}

motion::Profile::Description motion::Profile::description() const noexcept
{
    Description result;
    result.size = m_size;
    result.distance = m_distance;
    result.end_velocity = m_endVelocity;
    for (unsigned i = 0; i < m_size; ++i) {
        const auto& p = m_phases[i];
        result.phases[i] = { .duration = p.duration,
                             .jerk = p.jerk,
                             .position = p.state.position,
                             .velocity = p.state.velocity,
                             .acceleration = p.state.acceleration };
    }
    return result;
}
std::optional<motion::Profile> motion::Profile::fromDescription(const Description& value) noexcept
{
    if (value.size > value.phases.size()) {
        return {};
    }
    Profile result;
    const auto near = [](double a, double b) {
        return std::abs(a - b) <= POSITION_TOLERANCE * std::max({ 1.0, std::abs(a), std::abs(b) });
    };
    for (unsigned i = 0; i < value.size; ++i) {
        const auto& p = value.phases[i];
        if (!std::isfinite(p.duration) || !std::isfinite(p.jerk) || !std::isfinite(p.position) ||
            !std::isfinite(p.velocity) || !std::isfinite(p.acceleration) || p.duration <= 0 ||
            !near(p.position, result.m_state.position) || !near(p.velocity, result.m_state.velocity)) {
            return {};
        }
        // Acceleration may jump for a trapezoidal (unlimited jerk) profile.
        auto velocity = [&](double t) { return p.velocity + (p.acceleration * t) + (HALF * p.jerk * t * t); };
        if (velocity(0) < -POSITION_TOLERANCE || velocity(p.duration) < -POSITION_TOLERANCE) {
            return {};
        }
        if (p.jerk != 0) {
            auto t = -p.acceleration / p.jerk;
            if (t > 0 && t < p.duration && velocity(t) < -POSITION_TOLERANCE) {
                return {};
            }
        }
        result.append({ .duration = p.duration, .jerk = p.jerk, .acceleration = p.acceleration });
        if (!std::isfinite(result.m_state.position) || !std::isfinite(result.m_duration)) {
            return {};
        }
    }
    if (!std::isfinite(value.distance) || !std::isfinite(value.end_velocity) || value.distance < 0 ||
        value.end_velocity < 0 || !near(value.distance, result.m_state.position) ||
        !near(value.end_velocity, result.m_state.velocity)) {
        return {};
    }
    result.m_distance = value.distance;
    result.m_endVelocity = value.end_velocity;
    return result;
}
