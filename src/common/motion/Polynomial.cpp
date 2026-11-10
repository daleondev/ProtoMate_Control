#include "Polynomial.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace motion
{
    namespace
    {
        constexpr double HALF{ 0.5 };
        constexpr double EPSILON{ 128 * std::numeric_limits<double>::epsilon() };
        using BinomialRow = std::array<unsigned, Polynomial::DEGREE + 1>;
        constexpr std::array BINOMIAL{
            BinomialRow{ 1 },          BinomialRow{ 1, 1 },          BinomialRow{ 1, 2, 1 },
            BinomialRow{ 1, 3, 3, 1 }, BinomialRow{ 1, 4, 6, 4, 1 }, BinomialRow{ 1, 5, 10, 10, 5, 1 }
        };
        constexpr double ROOT_SEPARATION{ 8 * std::numeric_limits<double>::epsilon() };
        constexpr unsigned BISECTION_ITERATIONS{ 60 };

        unsigned degree(const Polynomial& p) noexcept
        {
            unsigned n{ Polynomial::DEGREE };
            while (n && p.coefficients[n] == 0) {
                --n;
            }
            return n;
        }

        double error(const Polynomial& p) noexcept
        {
            double total{ 1 };
            for (double c : p.coefficients) {
                total += std::abs(c);
            }
            return EPSILON * total;
        }

        std::size_t append_root(std::span<double, Polynomial::DEGREE> output,
                                double from,
                                double to,
                                double x,
                                std::size_t count) noexcept
        {
            if (x > from && x < to && count < output.size() &&
                (count == 0 || x - output[count - 1] > ROOT_SEPARATION)) {
                output[count++] = x;
            }
            return count;
        }

        double bisect(const Polynomial& polynomial, double low, double high) noexcept
        {
            double flow{ polynomial.at(low) };
            for (unsigned iteration{ 0 }; iteration < BISECTION_ITERATIONS; ++iteration) {
                const double middle{ low + ((high - low) * HALF) };
                const double value{ polynomial.at(middle) };
                if (middle == low || middle == high || value == 0) {
                    return middle;
                }
                if ((flow < 0) == (value < 0)) {
                    low = middle;
                    flow = value;
                }
                else {
                    high = middle;
                }
            }
            return low + ((high - low) * HALF);
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

    // Polynomial evaluation uses coordinate followed by optional derivative order.
    // Preserve this established mathematical API; out-of-range orders evaluate to zero.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    double Polynomial::at(double u, unsigned order) const noexcept
    {
        if (order > DEGREE) {
            return 0;
        }
        double value{ 0 };
        for (int i{ static_cast<int>(DEGREE) }; std::cmp_greater_equal(i, order); --i) {
            double c{ coefficients[static_cast<std::size_t>(i)] };
            for (unsigned j{ 0 }; j < order; ++j) {
                c *= i - static_cast<int>(j);
            }
            value = (value * u) + c;
        }
        return value;
    }

    Polynomial Polynomial::derivative() const noexcept
    {
        Polynomial result{};
        for (std::size_t i{ 1 }; i < coefficients.size(); ++i) {
            result.coefficients[i - 1] = coefficients[i] * static_cast<double>(i);
        }
        return result;
    }

    Polynomial Polynomial::restrict(double from, double to) const noexcept
    {
        Polynomial result{};
        const double width{ to - from };
        double width_power{ 1 };
        for (unsigned k{ 0 }; k <= DEGREE; ++k) {
            double shifted{ 0 };
            for (int j{ static_cast<int>(DEGREE) }; std::cmp_greater_equal(j, k); --j) {
                shifted = (shifted * from) + (coefficients[static_cast<unsigned>(j)] * BINOMIAL[j][k]);
            }
            result.coefficients[k] = shifted * width_power;
            width_power *= width;
        }
        return result;
    }

    Interval Polynomial::range(double from, double to, unsigned order) const noexcept
    {
        if (!finite() || !std::isfinite(from) || !std::isfinite(to) || from > to || order > DEGREE) {
            return { .minimum = std::numeric_limits<double>::quiet_NaN(),
                     .maximum = std::numeric_limits<double>::quiet_NaN() };
        }
        auto p{ *this };
        for (unsigned i{ 0 }; i < order; ++i) {
            p = p.derivative();
        }
        const auto shifted{ p.restrict(from, to) };
        const unsigned n{ DEGREE - order };
        Interval result{ .minimum = shifted.coefficients[0], .maximum = shifted.coefficients[0] };
        for (unsigned i{ 1 }; i <= n; ++i) {
            double b{ 0 };
            for (unsigned k{ 0 }; k <= i; ++k) {
                b += shifted.coefficients[k] * static_cast<double>(BINOMIAL[i][k]) / BINOMIAL[n][k];
            }
            result.minimum = std::min(result.minimum, b);
            result.maximum = std::max(result.maximum, b);
        }
        const double margin{ error(p) + error(shifted) };
        result.minimum = std::nextafter(result.minimum - margin, -std::numeric_limits<double>::infinity());
        result.maximum = std::nextafter(result.maximum + margin, std::numeric_limits<double>::infinity());
        return result;
    }

    std::size_t Polynomial::roots(std::span<double, DEGREE> output, double from, double to) const noexcept
    {
        if (!finite() || !std::isfinite(from) || !std::isfinite(to) || from >= to) {
            return 0;
        }
        const unsigned n{ degree(*this) };
        if (!n) {
            return 0;
        }
        if (n == 1) {
            const double root{ -coefficients[0] / coefficients[1] };
            if (root > from && root < to) {
                output[0] = root;
                return 1;
            }
            return 0;
        }
        std::array<double, DEGREE> turning{};
        const auto turns{ derivative().roots(turning, from, to) };
        std::size_t count{ 0 };
        double left{ from };
        double fleft{ at(left) };
        for (std::size_t interval{ 0 }; interval <= turns; ++interval) {
            const double right{ interval < turns ? turning[interval] : to };
            const double fright{ at(right) };
            if ((fleft < 0 && fright > 0) || (fleft > 0 && fright < 0)) {
                count = append_root(output, from, to, bisect(*this, left, right), count);
            }
            if (interval < turns && std::abs(fright) <= error(*this)) {
                count = append_root(output, from, to, right, count);
            }
            left = right;
            fleft = fright;
        }
        return count;
    }

    // Closed-form quintic Hermite basis coefficients, not tunable tolerances.
    // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    Polynomial hermite(KinematicState start, KinematicState end, double t) noexcept
    {
        const double v0{ start.velocity * t };
        const double v1{ end.velocity * t };
        const double a0{ start.acceleration * t * t };
        const double a1{ end.acceleration * t * t };
        const double d{ end.position - start.position };
        return { { start.position,
                   v0,
                   a0 * HALF,
                   (10 * d) - (6 * v0) - (4 * v1) - (1.5 * a0) + (0.5 * a1),
                   (-15 * d) + (8 * v0) + (7 * v1) + (1.5 * a0) - a1,
                   (6 * d) - (3 * v0) - (3 * v1) - (0.5 * a0) + (0.5 * a1) } };
    }
    // NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
}
