#pragma once

#include "common/motion/Profile.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace motion_bench::compact
{
    inline constexpr unsigned RUN_LENGTH = 128;
    inline constexpr unsigned CAPACITY = 512;
    inline constexpr double TICKS_PER_SECOND = 10'000'000.;
    inline constexpr double Q = 4294967296.;
    inline constexpr std::int64_t MAX_START = std::int64_t{100'000'000} << 32;
    inline constexpr std::int64_t MAX_INTERVAL = std::int64_t{1'000'000} << 32;
    inline constexpr std::int64_t MAX_ADD = std::int64_t{10'000} << 32;

    // Exact integer-grid representation: quadratic Q32 prediction and signed
    // one-tick corrections. No native layout is used as a wire format.
    struct Run
    {
        std::uint32_t first{};
        std::uint16_t count{};
        std::int64_t start{}, interval{}, add{};
        std::array<std::uint64_t, 2> plus{}, minus{};
    };
    static_assert(sizeof(Run) == 64);
    struct Program
    {
        std::array<Run, CAPACITY> runs{};
        std::uint32_t size{}, pulses{}, failed_fits{}, checked{};
    };

    inline std::uint32_t ceiling(std::int64_t value) noexcept
    {
        return std::uint32_t(value >> 32) + (std::uint32_t(value) != 0);
    }
    inline int correction(const Run& run, unsigned n) noexcept
    {
        return int((run.plus[n / 64] >> (n % 64)) & 1) -
               int((run.minus[n / 64] >> (n % 64)) & 1);
    }
    inline std::uint32_t tick(const Run& run, unsigned n) noexcept
    {
        const auto value = run.start + n * run.interval +
                           (std::int64_t(n) * (n - (n != 0)) / 2) * run.add;
        return ceiling(value) + correction(run, n);
    }
    inline bool valid(const Run& run) noexcept
    {
        if (!run.count || run.count > RUN_LENGTH || run.first > UINT32_MAX - run.count || run.start < 0 || run.start > MAX_START ||
            run.interval < 0 || run.interval > MAX_INTERVAL || run.add < -MAX_ADD || run.add > MAX_ADD)
            return false;
        for (unsigned word = 0; word < 2; ++word)
            if (run.plus[word] & run.minus[word])
                return false;
        // The coefficient limits bound all intermediate signed arithmetic below
        // INT64_MAX, including deliberately malformed combinations.
        std::uint32_t previous{};
        for (unsigned i = 0; i < run.count; ++i) {
            const auto q = run.start + i * run.interval +
                           (std::int64_t(i) * (i - (i != 0)) / 2) * run.add;
            if (q < 0 || q > MAX_START)
                return false;
            const auto raw = ceiling(q);
            if ((!raw && correction(run, i) < 0) || raw > 100'000'000U)
                return false;
            const auto current = raw + correction(run, i);
            if (current > 100'000'000U) return false;
            if (i && current <= previous)
                return false;
            previous = current;
        }
        return true;
    }

    // Cached analytic phase evaluator. Profile::timeAt supplies only three fit
    // points per attempted run; every emitted tick is independently certified.
    class Curve
    {
      public:
        explicit Curve(const motion::Profile& profile) : m_profile(profile), m_phases(profile.description())
        {
            for (unsigned i = 1; i < m_phases.size; ++i)
                m_starts[i] = m_starts[i - 1] + m_phases.phases[i - 1].duration;
            for (unsigned i = 0; i < m_phases.size; ++i) {
                const auto& p = m_phases.phases[i];
                m_cruise |= p.acceleration == 0 && p.jerk == 0 && p.velocity > 0;
                m_halfAcceleration[i] = p.acceleration * .5;
                m_sixthJerk[i] = p.jerk / 6;
            }
        }
        double time(double position) const noexcept { return m_profile.timeAt(position); }
        bool crossed(std::uint32_t ticks, double target) const noexcept
        {
            // At a zero-velocity endpoint, rounded position evaluation can
            // equal the terminal value several ticks too early. The analytic
            // endpoint time is authoritative (as in Profile::timeAt).
            if (target >= m_phases.distance)
                return ticks >= std::ceil(m_profile.duration() * TICKS_PER_SECOND);
            for (unsigned i = 0; m_cruise && i < m_phases.size; ++i) {
                if (i + 1 < m_phases.size && target > m_phases.phases[i + 1].position)
                    continue;
                const auto& p = m_phases.phases[i];
                if (p.acceleration == 0 && p.jerk == 0 && p.velocity > 0)
                    return ticks >= std::ceil(m_starts[i] * TICKS_PER_SECOND +
                                              (target - p.position) * (TICKS_PER_SECOND / p.velocity));
                break;
            }
            return position(ticks) >= target;
        }
        double position(std::uint32_t ticks) const noexcept
        {
            // Use the timer's exact integer divisor. Multiplying by the rounded
            // reciprocal can move an exact phase boundary one ULP to its left.
            const double time = ticks / TICKS_PER_SECOND;
            for (unsigned i = 0; i < m_phases.size; ++i) {
                const auto& p = m_phases.phases[i];
                if (time <= m_starts[i] + p.duration) {
                    const auto t = std::clamp(time - m_starts[i], 0., p.duration);
                    return p.position + t * (p.velocity + t * (m_halfAcceleration[i] + t * m_sixthJerk[i]));
                }
            }
            return m_phases.distance;
        }
        unsigned count() const noexcept { return unsigned(std::floor(m_phases.distance)); }
      private:
        const motion::Profile& m_profile;
        motion::Profile::Description m_phases;
        std::array<double, motion::Profile::MAX_PHASES> m_starts{};
        std::array<double, motion::Profile::MAX_PHASES> m_halfAcceleration{}, m_sixthJerk{};
        bool m_cruise{};
    };

    template<class Source>
    bool fit(const Source& curve, unsigned first, unsigned count, Run& run, std::uint32_t& checked)
    {
        run = {};
        run.first = first;
        run.count = static_cast<std::uint16_t>(count);
        if (count == 1) {
            // Close to rest, tiny position-rounding differences between the
            // analytic inverse and FMA evaluation can span several timer ticks.
            // Search the same crossing predicate instead of weakening it.
            std::uint32_t lo{}, hi = 100'000'000;
            if (!curve.crossed(hi, first + 1.))
                return false;
            while (lo < hi) {
                const auto mid = lo + (hi - lo) / 2;
                if (curve.crossed(mid, first + 1.)) hi = mid;
                else lo = mid + 1;
            }
            run.start = std::int64_t(lo) << 32;
            ++checked;
            return valid(run);
        }
        const auto scale = TICKS_PER_SECOND * Q;
        const auto start = curve.time(first + 1.) * scale;
        if (!std::isfinite(start) || start < 0 || start > double(MAX_START))
            return false;
        double interval{}, add{};
        if (count > 1) {
            const auto end = curve.time(first + count) * scale;
            interval = (end - start) / (count - 1);
            if (count > 2) {
                const unsigned mid = (count - 1) / 2;
                const auto middle = curve.time(first + mid + 1.) * scale;
                add = 2 * (interval - (middle - start) / mid) / (count - 1 - mid);
                interval = (middle - start) / mid - (mid - 1.) * add * .5;
            }
        }
        if (!std::isfinite(interval) || !std::isfinite(add) || interval < 0 ||
            interval > double(MAX_INTERVAL) || std::abs(add) > double(MAX_ADD))
            return false;
        run.start = std::llround(start);
        run.interval = std::llround(interval);
        run.add = std::llround(add);
        for (unsigned n = 0; n < count; ++n) {
            auto predicted = tick(run, n);
            const double target = first + n + 1.;
            const auto crossed = [&](std::uint32_t t) { return curve.crossed(t, target); };
            int change{};
            if (!crossed(predicted)) {
                change = 1;
                ++predicted;
                if (!crossed(predicted)) return false;
            }
            else if (predicted && crossed(predicted - 1)) {
                change = -1;
                --predicted;
                if (predicted && crossed(predicted - 1)) return false;
            }
            ++checked;
            if (change > 0)
                run.plus[n / 64] |= std::uint64_t{1} << (n % 64);
            if (change < 0)
                run.minus[n / 64] |= std::uint64_t{1} << (n % 64);
        }
        return valid(run);
    }

    template<class Source>
    bool compile(const Source& curve, Program& output)
    {
        output.size = output.pulses = output.failed_fits = output.checked = 0;
        const auto count = curve.count();
        std::uint32_t size{}, pulses{}, failed{}, checked{};
        const auto finish = [&](bool success) {
            output.size = size;
            output.pulses = pulses;
            output.failed_fits = failed;
            output.checked = checked;
            return success;
        };
        while (pulses < count) {
            if (size == output.runs.size())
                return finish(false);
            auto length = std::min<unsigned>(RUN_LENGTH, count - pulses);
            // Keep hot fitting state in private internal RAM. The completed
            // program may live in shared, non-cacheable external memory.
            Run run;
            while (!fit(curve, pulses, length, run, checked)) {
                ++failed;
                if (length == 1)
                    return finish(false);
                length /= 2;
            }
            output.runs[size++] = run;
            pulses += length;
        }
        return finish(true);
    }

    // Expansion is integer-only and bounded by output size. Consumers can call
    // this for an arbitrary DMA half without maintaining hidden mutable state.
    bool expand(const Program& program, unsigned first, std::span<std::uint32_t> out) noexcept;
}
