#include "Peripherals.hpp"
#include <algorithm>
#include <cerrno>
namespace simulation
{
    std::uint64_t Peripherals::elapsed(const Timer& t, std::uint64_t now) const
    {
        if (!t.running)
            return 0;
        const auto hz{ t.frequency / (std::uint64_t{ t.prescaler } + 1) };
        const auto delta{ now - t.origin };
        return delta / 1'000'000'000 * hz + (delta % 1'000'000'000) * hz / 1'000'000'000;
    }
    void Peripherals::capture(Timer& t, std::uint64_t now)
    {
        t.counter =
          static_cast<std::uint32_t>((t.counter + elapsed(t, now)) % (std::uint64_t{ t.reload } + 1));
        t.origin = now;
        t.cycles = 0;
    }
    void Peripherals::publish(unsigned i, std::uint64_t now)
    {
        const auto& t{ m_timers[i] };
        auto& s{ m_shared.timers[i] };
        release(s[0],
                static_cast<std::uint32_t>((t.counter + elapsed(t, now)) % (std::uint64_t{ t.reload } + 1)));
        release(s[1], t.prescaler);
        release(s[2], t.reload);
        release(s[3], t.frequency);
        release(s[4], std::uint32_t(t.running));
        release(s[5], t.version);
        release(s[6], static_cast<std::uint32_t>(t.notifications));
    }
    void Peripherals::execute(const Packet& q, Packet& r, std::uint64_t now)
    {
        const auto i{ q.arguments[0] };
        if (q.operation == Operation::PwmConfigure || q.operation == Operation::PwmStart ||
            q.operation == Operation::PwmStop) {
            if (i >= 3) {
                r.status = EINVAL;
                return;
            }
            auto& p{ m_pwm[i] };
            if (q.operation == Operation::PwmConfigure) {
                if (p.running || q.arguments[1] >= 176 || !q.arguments[2] ||
                    q.arguments[3] > q.arguments[2]) {
                    r.status = EINVAL;
                    return;
                }
                p.pin = static_cast<unsigned>(q.arguments[1]);
                p.period = q.arguments[2];
                p.high = q.arguments[3];
            }
            else if (q.operation == Operation::PwmStart) {
                if (p.running && !p.counted && !q.arguments[2])
                    return;
                if (p.running || !p.period || (q.arguments[2] && (p.high == 0 || p.high == p.period))) {
                    r.status = EINVAL;
                    return;
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
            }
            else {
                p.running = false;
                p.level = false;
            }
            release(m_shared.gpio[p.pin], std::uint32_t(p.level));
            release(m_shared.pwm[i][0], p.count);
            release(m_shared.pwm[i][1], std::uint64_t(p.running));
            release(m_shared.pwm[i][2], std::uint64_t(p.level));
            return;
        }
        if (i >= m_timers.size()) {
            r.status = EINVAL;
            return;
        }
        auto& t{ m_timers[i] };
        if (q.operation == Operation::TimerConfigure) {
            capture(t, now);
            if (q.arguments[1] == 0) {
                t.frequency = static_cast<std::uint32_t>(q.arguments[2]);
                t.prescaler = static_cast<std::uint32_t>(q.arguments[3]);
                t.reload = static_cast<std::uint32_t>(q.arguments[4]);
            }
            if (q.arguments[1] == 1)
                t.counter = static_cast<std::uint32_t>(q.arguments[2]);
            if (q.arguments[1] == 2)
                t.prescaler = static_cast<std::uint32_t>(q.arguments[2]);
            if (q.arguments[1] == 3)
                t.reload = static_cast<std::uint32_t>(q.arguments[2]);
            if (q.arguments[1] == 4) {
                t.reload = static_cast<std::uint32_t>(q.arguments[2]);
                t.counter = 0;
            }
            ++t.version;
        }
        else if (q.operation == Operation::TimerStart) {
            if (t.frequency / (std::uint64_t{ t.prescaler } + 1) == 0) {
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
        publish(static_cast<unsigned>(i), now);
    }
    std::uint64_t Peripherals::deadline(std::uint64_t now, std::uint64_t maximum) const
    {
        for (const auto& t : m_timers)
            if (t.running && t.interrupt) {
                const auto hz{ t.frequency / (std::uint64_t{ t.prescaler } + 1) };
                const auto period{ std::uint64_t{ t.reload } + 1 };
                const auto ticks{ (t.cycles + 1) * period - t.counter };
                const auto end{ t.origin + (ticks / hz) * 1'000'000'000 +
                                (ticks % hz * 1'000'000'000 + hz - 1) / hz };
                maximum = std::min(maximum, std::max(now, ((end + 99) / 100) * 100));
            }
        for (const auto& p : m_pwm)
            if (p.running)
                maximum = std::min(maximum, p.next);
        return maximum;
    }
    unsigned Peripherals::advance(std::uint64_t now)
    {
        unsigned flags{};
        for (unsigned i{}; i < m_timers.size(); ++i) {
            auto& t{ m_timers[i] };
            const auto cycles{ (t.counter + elapsed(t, now)) / (std::uint64_t{ t.reload } + 1) };
            if (t.running && t.interrupt && cycles > t.cycles) {
                flags |= 8U;
                t.notifications += cycles - t.cycles;
            }
            t.cycles = cycles;
            publish(i, now);
        }
        for (unsigned i{}; i < m_pwm.size(); ++i) {
            auto& p{ m_pwm[i] };
            if (p.running && p.next <= now) {
                p.level = !p.level;
                if (p.level) {
                    if (p.counted)
                        ++p.count;
                    p.next += p.high;
                }
                else {
                    p.next += p.period - p.high;
                    if (p.limit && p.count >= p.limit)
                        p.running = false;
                }
                release(m_shared.gpio[p.pin], std::uint32_t(p.level));
                release(m_shared.pwm[i][0], p.count);
                release(m_shared.pwm[i][1], std::uint64_t(p.running));
                release(m_shared.pwm[i][2], std::uint64_t(p.level));
                if (p.counted)
                    flags |= 16U;
            }
        }
        return flags;
    }
}
