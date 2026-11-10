#include "ProgressPath.hpp"
#include <limits>
namespace motion
{
    namespace
    {
        constexpr double CONTINUITY_EPS{ 1e-8 }, MIDPOINT{ .5 };
        constexpr unsigned MAX_CROSSING_ITERATIONS{ 56 }, NEWTON_ITERATIONS{ 12 };
        bool continuous_join(const ProgressPath& path, unsigned i) noexcept
        {
            const auto begin{ path.ends[i - 1] };
            const auto previous{ begin - (i > 1 ? path.ends[i - 2] : 0.) };
            const auto width{ path.ends[i] - begin };
            for (unsigned d{ 0 }; d < 3; ++d) {
                const auto a{ path.curves[i - 1].at(1, d) / std::pow(previous, int(d)) };
                const auto b{ path.curves[i].at(0, d) / std::pow(width, int(d)) };
                if (!std::isfinite(a) || !std::isfinite(b) ||
                    std::abs(a - b) > CONTINUITY_EPS * std::max({ 1., std::abs(a), std::abs(b) })) {
                    return false;
                }
            }
            return true;
        }
    }
    double ProgressPath::at(double p, unsigned derivative) const noexcept
    {
        if (!size || size > CAPACITY) {
            return 0;
        }
        unsigned i{};
        while (i + 1 < size && p > ends[i]) {
            ++i;
        }
        const auto begin{ i ? ends[i - 1] : 0. };
        const auto width{ ends[i] - begin };
        double scale{ 1 };
        for (unsigned d{ 0 }; d < derivative; ++d) {
            scale *= width;
        }
        return curves[i].at(std::clamp((p - begin) / width, 0., 1.), derivative) / scale;
    }
    Interval ProgressPath::range(unsigned derivative) const noexcept
    {
        Interval result{ .minimum = std::numeric_limits<double>::infinity(),
                         .maximum = -std::numeric_limits<double>::infinity() };
        for (unsigned i{ 0 }; i < size && i < CAPACITY; ++i) {
            auto r{ curves[i].range(0, 1, derivative) };
            const auto scale{ std::pow(ends[i] - (i ? ends[i - 1] : 0.), int(derivative)) };
            result.minimum = std::min(result.minimum, r.minimum / scale);
            result.maximum = std::max(result.maximum, r.maximum / scale);
        }
        return result;
    }
    bool ProgressPath::valid(double distance) const noexcept
    {
        if (!size || size > CAPACITY || !std::isfinite(distance) || distance <= 0 || ends[size - 1] != 1.) {
            return false;
        }
        for (unsigned i{ 0 }; i < size; ++i) {
            const auto begin{ i ? ends[i - 1] : 0. };
            if (!std::isfinite(ends[i]) || ends[i] <= begin || !curves[i].finite()) {
                return false;
            }
            const auto slope{ curves[i].range(0, 1, 1) };
            if (!slope.finite() || slope.minimum < -CONTINUITY_EPS) {
                return false;
            }
            if (i && !continuous_join(*this, i)) {
                return false;
            }
        }
        for (unsigned d{ 0 }; d <= 3; ++d) {
            if (!range(d).finite()) {
                return false;
            }
        }
        return std::abs(at(0)) < CONTINUITY_EPS && std::abs(at(1) - distance) < CONTINUITY_EPS;
    }
    std::array<double, 3> ProgressPath::dynamicBounds(Profile::Limits p) const noexcept
    {
        // Include the velocity increase while a local stop unwinds positive
        // acceleration: bounds nominal motion and every stopping continuation.
        const auto v{ p.velocity + (p.acceleration * p.acceleration / (2 * p.jerk)) };
        const auto a{ std::max(p.acceleration, p.deceleration) };
        const auto d1{ range(1).magnitude() };
        const auto d2{ range(2).magnitude() };
        const auto d3{ range(3).magnitude() };
        return { d1 * v, (d2 * v * v) + (d1 * a), (d3 * v * v * v) + (3 * d2 * v * a) + (d1 * p.jerk) };
    }
    double ProgressPath::crossing(double position, Cursor& cursor) const noexcept
    {
        if (position <= 0) {
            return 0;
        }
        if (position >= at(1)) {
            return 1;
        }
        unsigned i{ cursor.piece };
        if (i >= size) {
            i = 0;
        }
        while (i && position < curves[i].at(0)) {
            --i;
        }
        while (i + 1 < size && position > curves[i].at(1)) {
            ++i;
        }
        const auto begin{ i ? ends[i - 1] : 0. };
        const auto width{ ends[i] - begin };
        const auto& q{ curves[i] };
        double lo{ 0 };
        double hi{ 1 };
        // A target must produce identical bits in every independently prepared
        // bank; seeding from the previous pulse could shift a grid-boundary edge.
        double u{ std::clamp((position - q.at(0)) / (q.at(1) - q.at(0)), 0., 1.) };
        for (unsigned n{ 0 }; n < MAX_CROSSING_ITERATIONS; ++n) {
            const auto error{ q.at(u) - position };
            if (std::abs(error) <= 2 * std::numeric_limits<double>::epsilon() * std::max(1., position) ||
                hi - lo < 2 * std::numeric_limits<double>::epsilon()) {
                break;
            }
            if (error >= 0) {
                hi = u;
            }
            else {
                lo = u;
            }
            const auto d{ q.at(u, 1) };
            const auto next{ d > 0 ? u - (error / d) : std::numeric_limits<double>::infinity() };
            u = n < NEWTON_ITERATIONS && next > lo && next < hi ? next : (lo + hi) * MIDPOINT;
        }
        cursor.piece = i;
        cursor.progress = begin + (width * u);
        return cursor.progress;
    }
}
