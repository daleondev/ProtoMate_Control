#include "Peripherals.hpp"
#include <algorithm>
#include <cerrno>
namespace simulation
{
    namespace
    {
        // Wide intermediate products preserve fractional divider frequencies and
        // avoid overflow when a timer has been running for a long time.
        __extension__ using Wide = unsigned __int128;
        constexpr unsigned TIMER_INTERRUPT{ 8U }, PWM_INTERRUPT{ 16U };
        constexpr std::uint64_t TIMELINE_NS{ 100 };
        constexpr unsigned VERSION_FIELD{ 5 }, NOTIFICATIONS_FIELD{ 6 };
        constexpr std::uint64_t NS{ 1'000'000'000 };
        std::uint64_t saturate(Wide value)
        {
            return value > UINT64_MAX ? UINT64_MAX : static_cast<std::uint64_t>(value);
        }
    }
    std::uint64_t Peripherals::elapsed(const Timer& t, std::uint64_t now)
    {
        if (!t.running) {
            return 0;
        }
        const auto divisor{ NS * (std::uint64_t{ t.prescaler } + 1) };
        return static_cast<std::uint64_t>(Wide{ now - t.origin } * t.frequency / divisor);
    }
    void Peripherals::capture(Timer& t, std::uint64_t now)
    {
        t.counter =
          static_cast<std::uint32_t>((Wide{ t.counter } + elapsed(t, now)) % (std::uint64_t{ t.reload } + 1));
        t.origin = now;
        t.cycles = 0;
    }
    void Peripherals::publish(Publication publication)
    {
        const auto [i, now]{ publication };
        const auto& t{ m_timers[i] };
        auto& s{ m_shared.timers[i] };
        release(s[0],
                static_cast<std::uint32_t>((Wide{ t.counter } + elapsed(t, now)) %
                                           (std::uint64_t{ t.reload } + 1)));
        release(s[1], t.prescaler);
        release(s[2], t.reload);
        release(s[3], t.frequency);
        release(s[4], std::uint32_t(t.running));
        release(s[VERSION_FIELD], t.version);
        release(s[NOTIFICATIONS_FIELD], static_cast<std::uint32_t>(t.notifications));
    }
    bool Peripherals::startPwm(Pwm& p, const Packet& q, std::uint64_t now)
    {
        if (p.running || !p.period || q.arguments[2] > 1 || p.period > UINT64_MAX - now ||
            (q.arguments[2] && (p.high == 0 || p.high == p.period))) {
            return false;
        }
        p.running = true;
        p.count = 0;
        p.limit = q.arguments[1];
        p.counted = q.arguments[2] != 0;
        p.level = false;
        p.next = now + p.period;
        if (p.high == 0 || p.high == p.period) {
            p.next = UINT64_MAX;
            p.level = p.high != 0;
        }
        return true;
    }
    void Peripherals::executePwm(const Packet& q, Packet& r, std::uint64_t now)
    {
        const auto i{ q.arguments[0] };
        if (i >= m_pwm.size()) {
            r.status = EINVAL;
            return;
        }
        auto& p{ m_pwm[i] };
        if (q.operation == Operation::PwmConfigure) {
            if (p.running || q.arguments[1] >= m_shared.gpio.size() || !q.arguments[2] ||
                q.arguments[2] > UINT64_MAX / 2 || q.arguments[3] > q.arguments[2]) {
                r.status = EINVAL;
                return;
            }
            p.configured = true;
            p.pin = static_cast<unsigned>(q.arguments[1]);
            p.period = q.arguments[2];
            p.high = q.arguments[3];
        }
        else if (q.operation == Operation::PwmStart) {
            if (p.running && !p.counted && !q.arguments[2]) {
                return;
            }
            if (!startPwm(p, q, now)) {
                r.status = EINVAL;
                return;
            }
        }

        else {
            if (!p.configured) {
                return;
            }
            p.running = false;
            p.level = false;
        }
        m_output(p.pin, p.level, now);
        release(m_shared.pwm[i][0], p.count);
        release(m_shared.pwm[i][1], std::uint64_t(p.running));
        release(m_shared.pwm[i][2], std::uint64_t(p.level));
    }
    bool Peripherals::configureTimer(Timer& t, const Packet& q, std::uint64_t now)
    {
        const auto kind{ q.arguments[1] };
        if (kind > 4 || q.arguments[2] > UINT32_MAX ||
            (kind == 0 && (!q.arguments[2] || q.arguments[2] > NS || q.arguments[3] > UINT32_MAX ||
                           q.arguments[4] > UINT32_MAX))) {
            return false;
        }
        capture(t, now);
        if (q.arguments[1] == 0) {
            t.frequency = static_cast<std::uint32_t>(q.arguments[2]);
            t.prescaler = static_cast<std::uint32_t>(q.arguments[3]);
            t.reload = static_cast<std::uint32_t>(q.arguments[4]);
        }
        if (q.arguments[1] == 1) {
            t.counter = static_cast<std::uint32_t>(q.arguments[2]);
        }
        if (q.arguments[1] == 2) {
            t.prescaler = static_cast<std::uint32_t>(q.arguments[2]);
        }
        if (q.arguments[1] == 3) {
            t.reload = static_cast<std::uint32_t>(q.arguments[2]);
        }
        if (q.arguments[1] == 4) {
            t.reload = static_cast<std::uint32_t>(q.arguments[2]);
            t.counter = 0;
        }
        t.counter = static_cast<std::uint32_t>(t.counter % (std::uint64_t{ t.reload } + 1));
        ++t.version;
        return true;
    }
    void Peripherals::execute(const Packet& q, Packet& r, std::uint64_t now)
    {
        const auto i{ q.arguments[0] };
        if (q.operation == Operation::PwmConfigure || q.operation == Operation::PwmStart ||
            q.operation == Operation::PwmStop) {
            executePwm(q, r, now);
            return;
        }
        if (i >= m_timers.size()) {
            r.status = EINVAL;
            return;
        }
        auto& t{ m_timers[i] };
        if (q.operation == Operation::TimerConfigure) {
            if (!configureTimer(t, q, now)) {
                r.status = EINVAL;
                return;
            }
        }
        else if (q.operation == Operation::TimerStart) {
            if (!t.frequency || q.arguments[1] > 1) {
                r.status = EINVAL;
                return;
            }
            capture(t, now);
            t.running = true;
            t.interrupt = q.arguments[1] != 0;
            ++t.version;
        }
        else if (q.operation == Operation::TimerStop) {
            capture(t, now);
            t.running = false;
            t.interrupt = false;
            ++t.version;
        }
        publish({ .index = static_cast<unsigned>(i), .time_ns = now });
    }
    std::uint64_t Peripherals::deadline(std::uint64_t now, std::uint64_t maximum) const
    {
        for (const auto& t : m_timers) {
            if (t.running && t.interrupt) {
                const auto period{ std::uint64_t{ t.reload } + 1 };
                const auto ticks{ ((Wide{ t.cycles } + 1) * period) - t.counter };
                const auto numerator{ ticks * NS * (std::uint64_t{ t.prescaler } + 1) };
                const auto end{ Wide{ t.origin } + ((numerator + t.frequency - 1) / t.frequency) };
                maximum = std::min(
                  maximum, std::max(now, saturate(((end + TIMELINE_NS - 1) / TIMELINE_NS) * TIMELINE_NS)));
            }
        }
        for (const auto& p : m_pwm) {
            if (p.running) {
                maximum = std::min(maximum, p.next);
            }
        }
        return maximum;
    }
    unsigned Peripherals::advance(std::uint64_t now)
    {
        unsigned flags{};
        for (unsigned i{}; i < m_timers.size(); ++i) {
            auto& t{ m_timers[i] };
            const auto cycles{ static_cast<std::uint64_t>((Wide{ t.counter } + elapsed(t, now)) /
                                                          (std::uint64_t{ t.reload } + 1)) };
            if (t.running && t.interrupt && cycles > t.cycles) {
                flags |= TIMER_INTERRUPT;
                t.notifications += cycles - t.cycles;
            }
            t.cycles = cycles;
            publish({ .index = i, .time_ns = now });
        }
        for (unsigned i{}; i < m_pwm.size(); ++i) {
            auto& p{ m_pwm[i] };
            if (p.running && p.next <= now) {
                p.level = !p.level;
                if (p.level) {
                    if (p.counted) {
                        ++p.count;
                    }
                    p.next = saturate(Wide{ p.next } + p.high);
                }
                else {
                    p.next = saturate(Wide{ p.next } + p.period - p.high);
                    if (p.limit && p.count >= p.limit) {
                        p.running = false;
                    }
                }
                m_output(p.pin, p.level, now);
                release(m_shared.pwm[i][0], p.count);
                release(m_shared.pwm[i][1], std::uint64_t(p.running));
                release(m_shared.pwm[i][2], std::uint64_t(p.level));
                if (p.counted) {
                    flags |= PWM_INTERRUPT;
                }
            }
        }
        return flags;
    }
    void Peripherals::stop(std::uint64_t now) noexcept
    {
        for (unsigned i{}; i < m_timers.size(); ++i) {
            m_timers[i].running = false;
            m_timers[i].interrupt = false;
            release(m_shared.timers[i][4], 0U);
        }
        for (unsigned i{}; i < m_pwm.size(); ++i) {
            auto& p{ m_pwm[i] };
            if (p.running) {
                m_output(p.pin, false, now);
            }
            p.running = p.level = false;
            release(m_shared.pwm[i][1], std::uint64_t{});
            release(m_shared.pwm[i][2], std::uint64_t{});
        }
    }

}
