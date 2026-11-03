#include "PulseCursor.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace motion::trajectory
{
    PulseCursor::PulseCursor(const Capabilities& c, Counts position, std::uint64_t origin)
      : m_capabilities{ c }
      , m_position{ position }
      , m_begin{ origin }
      , m_end{ origin }
    {
        for (auto& axis : m_axes) {
            axis.last_tick = origin;
        }
        if (c.tick_ns != TICK_NS || !c.high_ticks || c.minimum_period_ticks <= c.high_ticks ||
            !c.direction_setup_ticks || !c.direction_hold_ticks) {
            m_error = Error::Configuration;
        }
    }
    Error PulseCursor::append(const Segment& s) noexcept
    {
        if (m_error != Error::None) {
            return m_error;
        }
        if (m_active) {
            return Error::Busy;
        }
        const auto error = validate(s, m_capabilities.limits);
        if (error != Error::None) {
            return m_error = error;
        }
        if (m_end > std::numeric_limits<std::uint64_t>::max() - s.ticks) {
            return m_error = Error::Timing;
        }
        if (m_started && !continuous(m_boundary, s.at(0))) {
            return m_error = Error::Discontinuity;
        }
        if (!m_started) {
            for (unsigned i = 0; i < 3; ++i) {
                const auto q = quantize(s.motors[i].at(0));
                if (!q || *q != m_position[i]) {
                    return m_error = Error::Stale;
                }
            }
        }
        m_segment = s;
        m_begin = m_end;
        m_end += s.ticks;
        m_boundary = s.at(1);
        m_active = true;
        m_started = true;
        for (unsigned i = 0; i < 3; ++i) {
            auto& a = m_axes[i];
            std::array<double, 5> roots{};
            const auto count = s.motors[i].derivative().roots(roots);
            a.divisions[0] = 0;
            std::copy_n(roots.begin(), count, a.divisions.begin() + 1);
            a.divisions[count + 1] = 1;
            a.count = count + 2;
            a.interval = 0;
            a.pending.reset();
            if (const auto e = advance(i); e != Error::None) {
                return m_error = e;
            }
        }
        return Error::None;
    }
    Error PulseCursor::advance(unsigned axis) noexcept
    {
        auto& a = m_axes[axis];
        const auto& p = m_segment.motors[axis];
        while (a.interval + 1 < a.count) {
            const auto to = a.divisions[a.interval + 1];
            const auto endpoint = quantize(p.at(to));
            if (!endpoint) {
                return endpoint.error();
            }
            if (*endpoint == m_position[axis]) {
                ++a.interval;
                continue;
            }
            const bool forward = *endpoint > m_position[axis];
            const double crossing = static_cast<double>(m_position[axis]) + (forward ? 0.5 : -0.5);
            double low = a.divisions[a.interval], high = to;
            // Fixed iterations, independent of displacement. Bracketing retains
            // the selected monotone branch even for zero endpoint derivatives.
            for (unsigned iteration = 0; iteration < 52; ++iteration) {
                const double middle = low + (high - low) * 0.5;
                if ((p.at(middle) < crossing) == forward) {
                    low = middle;
                }
                else {
                    high = middle;
                }
            }
            const double relative = high * static_cast<double>(m_segment.ticks);
            auto relativeTick = static_cast<std::uint64_t>(std::ceil(relative));
            // The bisection bracket can lie a few ulps above an exact tick.
            // Test the preceding integer tick against the original polynomial;
            // never accumulate rounded inter-pulse periods.
            if (relativeTick) {
                const double previous = p.at(static_cast<double>(relativeTick - 1) / m_segment.ticks);
                if (forward ? previous >= crossing : previous < crossing) {
                    --relativeTick;
                }
            }
            const auto tick = m_begin + relativeTick;
            if (tick > m_end || tick < m_begin) {
                return Error::Timing;
            }
            if (a.has_pulsed) {
                const auto gap = tick - a.last_tick;
                const std::uint64_t reversal = std::uint64_t(m_capabilities.high_ticks) +
                                               m_capabilities.direction_hold_ticks +
                                               m_capabilities.direction_setup_ticks;
                if (tick <= a.last_tick || gap < m_capabilities.minimum_period_ticks ||
                    (forward != a.direction && gap < reversal)) {
                    return Error::Timing;
                }
            }
            // Account for the first direction setup at the execution origin.
            else if (tick < a.last_tick || tick - a.last_tick < m_capabilities.direction_setup_ticks) {
                return Error::Timing;
            }
            a.pending = Pulse{ tick, static_cast<std::uint8_t>(axis), forward };
            return Error::None;
        }
        a.pending.reset();
        return Error::None;
    }
    std::expected<std::optional<Pulse>, Error> PulseCursor::next() noexcept
    {
        if (m_error != Error::None) {
            return std::unexpected(m_error);
        }
        if (!m_active) {
            return std::optional<Pulse>{};
        }
        unsigned selected = 3;
        for (unsigned i = 0; i < 3; ++i) {
            if (m_axes[i].pending &&
                (selected == 3 || m_axes[i].pending->tick < m_axes[selected].pending->tick)) {
                selected = i;
            }
        }
        if (selected == 3) {
            m_active = false;
            return std::optional<Pulse>{};
        }
        auto& a = m_axes[selected];
        const auto pulse = *a.pending;
        m_position[selected] += pulse.forward ? 1 : -1;
        a.last_tick = pulse.tick;
        a.direction = pulse.forward;
        a.has_pulsed = true;
        if (const auto e = advance(selected); e != Error::None) {
            m_error = e;
            return std::unexpected(e);
        }
        return std::optional{ pulse };
    }
}
