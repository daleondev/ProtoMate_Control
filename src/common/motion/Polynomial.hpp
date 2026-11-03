#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace motion
{
    struct Interval
    {
        double minimum{}, maximum{};
        [[nodiscard]] bool finite() const noexcept;
        [[nodiscard]] double magnitude() const noexcept;
    };

    // Power coefficients in normalized time u=[0,1]. Derivatives here are
    // with respect to u; divide by physical duration^order for SI rates.
    struct Polynomial
    {
        std::array<double, 6> coefficients{};
        [[nodiscard]] double at(double u, unsigned derivative = 0) const noexcept;
        [[nodiscard]] bool finite() const noexcept;
        [[nodiscard]] Polynomial derivative() const noexcept;
        [[nodiscard]] Polynomial restrict(double from, double to) const noexcept;
        // Bernstein convex-hull enclosure, including a floating-point margin.
        [[nodiscard]] Interval range(double from = 0, double to = 1,
                                     unsigned derivative = 0) const noexcept;
        // Ordered real roots strictly inside (from,to), including even roots.
        // The caller supplies space for five roots. No allocation or recursion
        // dependent on input magnitude; degree bounds all recursion.
        [[nodiscard]] std::size_t roots(std::span<double, 5> output,
                                        double from = 0, double to = 1) const noexcept;
    };

    struct KinematicState
    {
        double position{}, velocity{}, acceleration{};
    };
    [[nodiscard]] Polynomial hermite(KinematicState start, KinematicState end,
                                     double seconds) noexcept;
}
