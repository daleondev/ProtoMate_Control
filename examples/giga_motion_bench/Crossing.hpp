#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace motion_bench
{
    // Experimental only: safeguarded Newton followed by an integer-grid
    // certificate. No float conversion, relaxed tolerance or accumulated period.
    struct Crossing
    {
        std::array<double, 6> c{};
        std::uint32_t ticks{};
        bool forward{ true };
        double value(double x) const noexcept
        {
            return ((((c[5] * x + c[4]) * x + c[3]) * x + c[2]) * x + c[1]) * x + c[0];
        }
        double derivative(double x) const noexcept
        {
            return (((5 * c[5] * x + 4 * c[4]) * x + 3 * c[3]) * x + 2 * c[2]) * x + c[1];
        }
        bool crossed(std::uint32_t tick, double target) const noexcept
        {
            const double y = value(double(tick) / ticks);
            return forward ? y >= target : y < target;
        }
        struct Result
        {
            std::uint32_t tick{}, evaluations{};
            bool fallback{};
        };
        Result solve(double target, double from, double to, double guess) const noexcept
        {
            const auto first = std::uint32_t(std::ceil(from * ticks));
            const auto last = std::uint32_t(std::ceil(to * ticks));
            double low = from, high = to;
            double x = std::clamp(guess, low, high);
            Result result;
            for (unsigned i = 0; i < 8; ++i) {
                const double y = value(x);
                ++result.evaluations;
                auto candidate = std::clamp(std::uint32_t(std::ceil(x * ticks)), first, last);
                if (crossed(candidate, target) && (candidate == first || !crossed(candidate - 1, target))) {
                    result.tick = candidate;
                    return result;
                }
                if ((y < target) == forward)
                    low = x;
                else
                    high = x;
                const double slope = derivative(x);
                const double next = x - (y - target) / slope;
                x = std::isfinite(next) && next > low && next < high ? next : low + (high - low) * .5;
            }
            // Strictly bounded fallback (at most 32 iterations) with the same
            // integer-crossing predicate as the certificate above.
            result.fallback = true;
            auto a = first, b = last;
            while (a < b) {
                auto middle = a + (b - a) / 2;
                ++result.evaluations;
                if (crossed(middle, target))
                    b = middle;
                else
                    a = middle + 1;
            }
            result.tick = a;
            return result;
        }
    };
}
