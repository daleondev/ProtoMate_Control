#include "MotionProfile.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace motion
{
    namespace
    {
        Profile::State advance(Profile::State s, double t, double jerk) noexcept
        {
            return { s.position + t * (s.velocity + t * (0.5 * s.acceleration + t * jerk / 6.0)),
                     s.velocity + t * (s.acceleration + 0.5 * t * jerk),
                     s.acceleration + t * jerk };
        }
        double changeDuration(double delta, double acceleration, double jerk) noexcept
        {
            delta = std::abs(delta);
            if (jerk == 0.0)
                return delta / acceleration;
            const auto ramp{ std::min(acceleration / jerk, std::sqrt(delta / jerk)) };
            return ramp + delta / (jerk * ramp);
        }
        double changeDistance(double from, double to, double acceleration, double jerk) noexcept
        {
            if (from == to)
                return 0.0;
            return 0.5 * (from + to) * changeDuration(to - from, acceleration, jerk);
        }
    }

    void Profile::append(double duration, double jerk, double acceleration) noexcept
    {
        if (duration <= 0.0)
            return;
        m_state.acceleration = acceleration;
        const auto next{ advance(m_state, duration, jerk) };
        m_phases[m_size++] = { m_duration, duration, jerk, next.position, m_state };
        m_state = next;
        m_duration += duration;
    }

    void Profile::changeVelocity(double target, double acceleration, double jerk) noexcept
    {
        const auto difference{ target - m_state.velocity };
        if (difference == 0.0)
            return;
        const auto sign{ difference > 0.0 ? 1.0 : -1.0 };
        const auto delta{ std::abs(difference) };
        if (jerk == 0.0) {
            append(delta / acceleration, 0.0, sign * acceleration);
        }
        else {
            const auto ramp{ std::min(acceleration / jerk, std::sqrt(delta / jerk)) };
            const auto peak{ jerk * ramp };
            append(ramp, sign * jerk, 0.0);
            append(std::max(0.0, delta / peak - ramp), 0.0, sign * peak);
            append(ramp, -sign * jerk, sign * peak);
        }
        m_state.velocity = target;
        m_state.acceleration = 0.0;
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
            !std::isfinite(start_acceleration))
            return std::nullopt;

        Profile result;
        result.m_state = { 0.0, start_velocity, start_acceleration };
        result.m_distance = distance;
        result.m_endVelocity = end_velocity;
        // A live splice retains acceleration as well as velocity. Bring that
        // acceleration continuously to zero before the usual seven phases.
        if (start_acceleration != 0.0 && limits.jerk > 0.0) {
            if (start_acceleration > limits.acceleration * (1.0 + 1e-10) ||
                -start_acceleration > limits.deceleration * (1.0 + 1e-10))
                return std::nullopt;
            result.append(std::abs(start_acceleration) / limits.jerk,
                          -std::copysign(limits.jerk, start_acceleration),
                          start_acceleration);
            if (result.m_state.velocity < 0.0 || result.m_state.position >= distance)
                return std::nullopt;
        }
        result.m_state.acceleration = 0.0;
        if (result.m_state.velocity > std::max(limits.velocity, end_velocity)) {
            const auto available{ distance - result.m_state.position };
            const auto inherited{ result.m_state.velocity };
            const auto ceiling{ std::max(limits.velocity, end_velocity) };
            const auto via = [&](double cruise) {
                return changeDistance(inherited, cruise, limits.deceleration, limits.jerk) +
                       changeDistance(cruise, end_velocity, limits.deceleration, limits.jerk);
            };
            if (via(ceiling) > available) {
                // A short successor may need one continuous deceleration. Do
                // not insist on flattening acceleration at its cruise speed:
                // that would reject an otherwise feasible high-speed blend.
                if (via(end_velocity) > available * (1.0 + 1e-12))
                    return std::nullopt;
                auto low{ end_velocity }, high{ ceiling };
                for (unsigned i = 0; i < 64U; ++i) {
                    const auto middle{ (low + high) * 0.5 };
                    if (via(middle) <= available)
                        low = middle;
                    else
                        high = middle;
                }
                result.changeVelocity(low, limits.deceleration, limits.jerk);
                const auto remaining{ std::max(
                  0.0,
                  distance - result.m_state.position -
                    changeDistance(low, end_velocity, limits.deceleration, limits.jerk)) };
                if (low > 0.0)
                    result.append(remaining / low, 0.0, 0.0);
                result.changeVelocity(end_velocity, limits.deceleration, limits.jerk);
                if (!std::isfinite(result.m_duration) || result.m_duration <= 0.0 ||
                    !std::isfinite(result.m_state.position) ||
                    std::abs(result.m_state.position - distance) > 1e-9 * std::max(distance, 1e-9))
                    return std::nullopt;
                return result;
            }
            result.changeVelocity(ceiling, limits.deceleration, limits.jerk);
            if (result.m_state.position >= distance)
                return std::nullopt;
        }
        const auto from{ result.m_state.velocity };
        const auto remaining{ distance - result.m_state.position };
        const auto minimum_peak{ std::max(from, end_velocity) };
        // A blend can explicitly request a junction faster than the previous
        // command's cruise speed. Preserve an inherited higher velocity too.
        const auto maximum_peak{ std::max(limits.velocity, minimum_peak) };
        const auto required = [&](double peak) {
            return changeDistance(from, peak, limits.acceleration, limits.jerk) +
                   changeDistance(peak, end_velocity, limits.deceleration, limits.jerk);
        };
        const auto minimum_distance{ required(minimum_peak) };
        if (!std::isfinite(minimum_distance) || minimum_distance > remaining * (1.0 + 1e-12))
            return std::nullopt;
        auto low{ minimum_peak }, high{ maximum_peak };
        for (unsigned i = 0; i < 64U; ++i) {
            const auto middle{ low + (high - low) * 0.5 };
            if (required(middle) > remaining)
                high = middle;
            else
                low = middle;
        }
        const auto peak{ required(maximum_peak) <= remaining ? maximum_peak : low };
        if (peak <= 0.0)
            return std::nullopt;
        const auto cruise{ std::max(0.0, remaining - required(peak)) / peak };
        result.changeVelocity(peak, limits.acceleration, limits.jerk);
        result.append(cruise, 0.0, 0.0);
        result.changeVelocity(end_velocity, limits.deceleration, limits.jerk);
        if (!std::isfinite(result.m_duration) || !std::isfinite(result.m_state.position) ||
            result.m_duration <= 0.0 ||
            std::abs(result.m_state.position - distance) > 1e-9 * std::max(distance, 1e-9))
            return std::nullopt;
        return result;
    }

    Profile::State Profile::at(double time) const noexcept
    {
        if (time >= m_duration)
            return { m_distance, m_endVelocity, 0.0 };
        for (unsigned i = 0; i < m_size; ++i) {
            const auto& phase{ m_phases[i] };
            if (time <= phase.start + phase.duration)
                return advance(phase.state, std::clamp(time - phase.start, 0.0, phase.duration), phase.jerk);
        }
        return { m_distance, m_endVelocity, 0.0 };
    }

    double Profile::timeAt(double position, double hint) const noexcept
    {
        if (position <= 0.0)
            return 0.0;
        if (position >= m_distance)
            return m_duration;
        for (unsigned i = 0; i < m_size; ++i) {
            const auto& p{ m_phases[i] };
            const auto end{ p.end_position };
            if (position > end && i + 1U < m_size)
                continue;
            const auto distance{ std::max(0.0, position - p.state.position) };
            if (p.jerk == 0.0) {
                const auto denominator{ p.state.velocity +
                                        std::sqrt(std::max(0.0,
                                                           p.state.velocity * p.state.velocity +
                                                             2.0 * p.state.acceleration * distance)) };
                return p.start +
                       std::clamp(denominator > 0.0 ? 2.0 * distance / denominator : 0.0, 0.0, p.duration);
            }
            if (p.state.velocity == 0.0 && p.state.acceleration == 0.0 && p.jerk > 0.0)
                return p.start + std::clamp(std::cbrt(6.0 * distance / p.jerk), 0.0, p.duration);
            // Safeguarded Newton inversion of a monotone cubic; fixed bound and
            // no allocation, suitable for the hardware scheduler's refill path.
            double lo{}, hi{ p.duration }, t{ p.duration * distance / (end - p.state.position) };
            if (hint > p.start && hint < p.start + p.duration) {
                const auto local{ hint - p.start };
                const auto prior{ advance(p.state, local, p.jerk) };
                if (prior.velocity > 0.0 && prior.position <= position)
                    t = std::clamp(local + (position - prior.position) / prior.velocity, lo, hi);
            }
            for (unsigned iteration = 0; iteration < 48U; ++iteration) {
                const auto state{ advance(p.state, t, p.jerk) };
                const auto error{ state.position - position };
                // A picosecond residual is far below the 100 ns timer tick.
                // Avoid bisecting after floating-point position has converged.
                if (std::abs(error) <= std::max(state.velocity, 1e-20) * 1e-12)
                    break;
                if (error > 0.0)
                    hi = t;
                else
                    lo = t;
                if (hi - lo < 1e-12)
                    break;
                const auto next{ state.velocity > 0.0 ? t - error / state.velocity : -1.0 };
                if (next == t)
                    break;
                t = next > lo && next < hi ? next : lo + (hi - lo) * 0.5;
            }
            return p.start + t;
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
        for (unsigned i = 0; i < m_size; ++i)
            examine(m_phases[i].state.position);
        return result;
    }
}
