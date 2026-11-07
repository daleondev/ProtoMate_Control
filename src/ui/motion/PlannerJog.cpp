#include "Planner.hpp"
#include <algorithm>
#include <cmath>

namespace ui::motion
{
    namespace tr = ::motion::trajectory;
    std::expected<tr::FiniteProgram, RobotError> Planner::pathProgram(double acceleration,
                                                                      double deceleration,
                                                                      double jerk)
    {
        if (!m_prepared || !m_cartesianPath || !m_pieces || m_pieces > ::motion::ProgressPath::CAPACITY)
            return std::unexpected(RobotError::Capacity);
        tr::FiniteProgram out;
        out.kind = tr::ProgramKind::CartesianJog;
        out.limits = m_model->motorLimits();
        const auto q0 = m_model->joints(m_seed);
        const auto scales = m_model->stepsPerJointUnit();
        for (unsigned joint = 0; joint < 3; ++joint) {
            const auto motor = m_model->configuration().motor_for_joint[joint];
            const auto total =
              (m_workspace->pieces[m_pieces - 1].joints[joint].at(1) - q0[joint]) * scales[joint];
            const auto rounded = tr::quantize(total);
            if (!rounded)
                return std::unexpected(rounded.error());
            const auto distance = *rounded;
            out.displacement[motor] = distance;
            const auto sign = distance >= 0 ? 1. : -1.;
            auto& path = out.path[motor];
            if (distance)
                path.size = unsigned(m_pieces);
            for (unsigned i = 0; i < m_pieces; ++i) {
                auto& piece = m_workspace->pieces[i];
                auto poly = piece.joints[joint];
                poly.coefficients[0] -= q0[joint];
                for (auto& c : poly.coefficients)
                    c *= scales[joint];
                if (!distance) {
                    if (poly.range().magnitude() > .50000001)
                        return std::unexpected(RobotError::Unsupported);
                    piece.joints[joint] = { { q0[joint] } };
                    continue;
                }
                // One endpoint quantization correction over the whole path,
                // preserving C2 joins. Numerical and grid error stay separate.
                const double correction = double(distance) - total;
                poly.coefficients[0] += correction * piece.begin;
                poly.coefficients[1] += correction * (piece.end - piece.begin);
                for (unsigned c = 0; c < 6; ++c)
                    piece.joints[joint].coefficients[c] = poly.coefficients[c] / scales[joint];
                piece.joints[joint].coefficients[0] += q0[joint];
                for (auto& c : poly.coefficients)
                    c *= sign;
                path.curves[i] = poly;
                path.ends[i] = piece.end;
            }
            if (distance && !path.valid(std::abs(double(distance))))
                return std::unexpected(RobotError::Unsupported); // reversal: stop at this path boundary
        }
        for (unsigned i = 0; i < m_pieces; ++i) {
            unsigned budget = 8192;
            const auto error = certify(m_workspace->pieces[i].joints, nullptr, 0, 1, 0, budget);
            if (error != RobotError::None)
                return std::unexpected(error);
        }
        if (std::ranges::all_of(out.displacement, [](auto d) { return d == 0; }))
            return std::unexpected(RobotError::PositionLimit);
        auto limits = scalarLimits(1);
        double stretch = 1;
        for (unsigned i = 0; i < 3; ++i) {
            auto& physical = out.limits[i];
            physical.acceleration *= acceleration;
            physical.deceleration *= deceleration;
            physical.jerk *= jerk;
            if (!out.path[i].size)
                continue;
            const auto bounds = out.path[i].dynamicBounds(limits);
            stretch =
              std::max({ stretch,
                         bounds[0] / physical.velocity,
                         std::sqrt(bounds[1] / std::min(physical.acceleration, physical.deceleration)),
                         std::cbrt(bounds[2] / physical.jerk) });
        }
        stretch *= 1.01;
        limits.velocity /= stretch;
        limits.acceleration /= stretch * stretch;
        limits.deceleration /= stretch * stretch;
        limits.jerk /= stretch * stretch * stretch;
        const auto law = ::motion::Profile::create(1, limits);
        if (!law || law->duration() > 1e6)
            return std::unexpected(RobotError::Timing);
        out.progress = law->description();
        out.progress_limits = limits;
        return out;
    }
    std::expected<tr::FiniteProgram, RobotError> Planner::cartesianJog(const tr::Counts& counts,
                                                                       std::array<double, 3> direction,
                                                                       double speed,
                                                                       double acceleration,
                                                                       double deceleration,
                                                                       double jerk)
    {
        unsigned axis = 3;
        for (unsigned i = 0; i < 3; ++i) {
            if (!std::isfinite(direction[i]) || (direction[i] != 0 && std::abs(direction[i]) != 1))
                return std::unexpected(RobotError::InvalidInput);
            if (direction[i]) {
                if (axis != 3)
                    return std::unexpected(RobotError::InvalidInput);
                axis = i;
            }
        }
        for (const auto factor : { speed, acceleration, deceleration, jerk })
            if (!std::isfinite(factor) || factor <= 0 || factor > 1)
                return std::unexpected(RobotError::InvalidInput);
        if (axis == 3)
            return std::unexpected(RobotError::InvalidInput);
        tr::Positions positions{};
        for (unsigned i = 0; i < 3; ++i)
            positions[i] = counts[i];
        const auto fk = m_model->kinematics().forward(m_model->toJoints(positions));
        if (!fk)
            return std::unexpected(RobotError::Unreachable);
        const auto from = fk->position;
        const auto xyz = [](CartesianPosition p) { return std::array{ p.x.get(), p.y.get(), p.z.get() }; };
        const auto begin = xyz(from);
        const auto edge = xyz(direction[axis] > 0 ? m_model->configuration().workspace_maximum
                                                  : m_model->configuration().workspace_minimum);
        const auto bound = direction[axis] * (edge[axis] - begin[axis]);
        if (!std::isfinite(bound) || bound <= 0)
            return std::unexpected(RobotError::Workspace);
        auto attempt = [&](double length) -> std::expected<tr::FiniteProgram, RobotError> {
            auto end = begin;
            end[axis] += direction[axis] * length;
            Move move{ .kind = MoveKind::Linear,
                       .target = { pnm::units::Distance::create(end[0]),
                                   pnm::units::Distance::create(end[1]),
                                   pnm::units::Distance::create(end[2]) },
                       .speed = speed };
            const auto error = prepare(move, counts);
            if (error != RobotError::None)
                return std::unexpected(error);
            return pathProgram(acceleration, deceleration, jerk);
        };
        // Find the first certified connected interval. Never skip over a
        // singularity, workspace violation, reversal or representation limit.
        double good = 0, bad = std::min(.001, bound);
        std::optional<tr::FiniteProgram> best;
        RobotError error = RobotError::Workspace;
        for (unsigned n = 0; n < 24; ++n) {
            if (m_stop.stop_requested())
                return std::unexpected(RobotError::Cancelled);
            auto candidate = attempt(bad);
            if (!candidate) {
                error = candidate.error();
                break;
            }
            best = *candidate;
            good = bad;
            if (good == bound)
                return *best;
            bad = std::min(bound, good * 2);
        }
        for (unsigned n = 0; n < 10 && bad - good > 1e-6; ++n) {
            if (m_stop.stop_requested())
                return std::unexpected(RobotError::Cancelled);
            const auto middle = (good + bad) / 2;
            auto candidate = attempt(middle);
            if (candidate) {
                best = *candidate;
                good = middle;
            }
            else {
                error = candidate.error();
                bad = middle;
            }
        }
        return best ? std::expected<tr::FiniteProgram, RobotError>{ *best } : std::unexpected(error);
    }
}
