#include "Polynomial.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace motion
{
    namespace
    {
        constexpr double EPSILON = 128 * std::numeric_limits<double>::epsilon();
        constexpr unsigned BINOMIAL[6][6]{ { 1 },          { 1, 1 },          { 1, 2, 1 },
                                           { 1, 3, 3, 1 }, { 1, 4, 6, 4, 1 }, { 1, 5, 10, 10, 5, 1 } };

        unsigned degree(const Polynomial& p) noexcept
        {
            unsigned n = 5;
            while (n && p.coefficients[n] == 0) {
                --n;
            }
            return n;
        }

        double error(const Polynomial& p) noexcept
        {
            double total = 1;
            for (double c : p.coefficients) {
                total += std::abs(c);
            }
            return EPSILON * total;
        }
    }

    bool Interval::finite() const noexcept
    {
        return std::isfinite(minimum) && std::isfinite(maximum) && minimum <= maximum;
    }
    double Interval::magnitude() const noexcept { return std::max(std::abs(minimum), std::abs(maximum)); }

    bool Polynomial::finite() const noexcept
    {
        return std::ranges::all_of(coefficients, [](double c) { return std::isfinite(c); });
    }

    double Polynomial::at(double u, unsigned order) const noexcept
    {
        if (order > 5) {
            return 0;
        }
        double value = 0;
        for (int i = 5; i >= static_cast<int>(order); --i) {
            double c = coefficients[static_cast<std::size_t>(i)];
            for (unsigned j = 0; j < order; ++j) {
                c *= i - static_cast<int>(j);
            }
            value = value * u + c;
        }
        return value;
    }

    Polynomial Polynomial::derivative() const noexcept
    {
        Polynomial result;
        for (std::size_t i = 1; i < coefficients.size(); ++i) {
            result.coefficients[i - 1] = coefficients[i] * static_cast<double>(i);
        }
        return result;
    }

    Polynomial Polynomial::restrict(double from, double to) const noexcept
    {
        Polynomial result;
        const double width = to - from;
        double width_power = 1;
        for (unsigned k = 0; k <= 5; ++k) {
            double shifted = 0;
            for (int j = 5; j >= static_cast<int>(k); --j) {
                shifted = shifted * from + coefficients[static_cast<unsigned>(j)] * BINOMIAL[j][k];
            }
            result.coefficients[k] = shifted * width_power;
            width_power *= width;
        }
        return result;
    }

    Interval Polynomial::range(double from, double to, unsigned order) const noexcept
    {
        if (!finite() || !std::isfinite(from) || !std::isfinite(to) || from > to || order > 5) {
            return { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN() };
        }
        auto p = *this;
        for (unsigned i = 0; i < order; ++i) {
            p = p.derivative();
        }
        const auto shifted = p.restrict(from, to);
        const unsigned n = 5 - order;
        Interval result{ shifted.coefficients[0], shifted.coefficients[0] };
        for (unsigned i = 1; i <= n; ++i) {
            double b = 0;
            for (unsigned k = 0; k <= i; ++k) {
                b += shifted.coefficients[k] * static_cast<double>(BINOMIAL[i][k]) / BINOMIAL[n][k];
            }
            result.minimum = std::min(result.minimum, b);
            result.maximum = std::max(result.maximum, b);
        }
        const double margin = error(p) + error(shifted);
        result.minimum = std::nextafter(result.minimum - margin, -std::numeric_limits<double>::infinity());
        result.maximum = std::nextafter(result.maximum + margin, std::numeric_limits<double>::infinity());
        return result;
    }

    std::size_t Polynomial::roots(std::span<double, 5> output, double from, double to) const noexcept
    {
        if (!finite() || !std::isfinite(from) || !std::isfinite(to) || from >= to) {
            return 0;
        }
        const unsigned n = degree(*this);
        if (!n) {
            return 0;
        }
        if (n == 1) {
            const double root = -coefficients[0] / coefficients[1];
            if (root > from && root < to) {
                output[0] = root;
                return 1;
            }
            return 0;
        }
        std::array<double, 5> turning{};
        const auto turns = derivative().roots(turning, from, to);
        std::size_t count = 0;
        const auto append = [&](double x) {
            if (x > from && x < to && count < output.size() &&
                (count == 0 || x - output[count - 1] > 8 * std::numeric_limits<double>::epsilon())) {
                output[count++] = x;
            }
        };
        double left = from;
        double fleft = at(left);
        for (std::size_t interval = 0; interval <= turns; ++interval) {
            const double right = interval < turns ? turning[interval] : to;
            const double fright = at(right);
            if ((fleft < 0 && fright > 0) || (fleft > 0 && fright < 0)) {
                double low = left, high = right, flow = fleft;
                for (unsigned iteration = 0; iteration < 60; ++iteration) {
                    const double middle = low + (high - low) * 0.5;
                    const double value = at(middle);
                    if (middle == low || middle == high || value == 0) {
                        low = high = middle;
                        break;
                    }
                    if ((flow < 0) == (value < 0)) {
                        low = middle;
                        flow = value;
                    }
                    else {
                        high = middle;
                    }
                }
                append(low + (high - low) * 0.5);
            }
            if (interval < turns && std::abs(fright) <= error(*this)) {
                append(right);
            }
            left = right;
            fleft = fright;
        }
        return count;
    }

    Polynomial hermite(KinematicState start, KinematicState end, double t) noexcept
    {
        const double v0 = start.velocity * t, v1 = end.velocity * t;
        const double a0 = start.acceleration * t * t, a1 = end.acceleration * t * t;
        const double d = end.position - start.position;
        return { { start.position,
                   v0,
                   a0 * 0.5,
                   10 * d - 6 * v0 - 4 * v1 - 1.5 * a0 + 0.5 * a1,
                   -15 * d + 8 * v0 + 7 * v1 + 1.5 * a0 - a1,
                   6 * d - 3 * v0 - 3 * v1 - 0.5 * a0 + 0.5 * a1 } };
    }
}
