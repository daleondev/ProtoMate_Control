#include "Planner.hpp"
#include <algorithm>
#include <cmath>

namespace ui::motion
{
    namespace tr = ::motion::trajectory;
    namespace
    {
        constexpr unsigned CERTIFICATION_BUDGET{ 8192 }, EXPANSION_STEPS{ 24 }, REFINEMENT_STEPS{ 10 };
        constexpr double HALF_STEP_TOLERANCE{ .50000001 }, RETIMING_MARGIN{ 1.01 }, MAX_JOG_SECONDS{ 1e6 },
          INITIAL_JOG_METRES{ .001 }, MIN_JOG_INTERVAL{ 1e-6 };
        template<class Attempt>
        std::expected<tr::FiniteProgram, RobotError> find_jog_interval(double bound,
                                                                       const std::stop_token& stop,
                                                                       const Attempt& attempt)
        {
            // Find the first certified connected interval. Never skip over a
            // singularity, workspace violation, reversal or representation limit.
            double good{ 0 };
            double bad{ std::min(INITIAL_JOG_METRES, bound) };
            std::optional<tr::FiniteProgram> best{};
            RobotError error{ RobotError::Workspace };
            for (unsigned n{ 0 }; n < EXPANSION_STEPS; ++n) {
                if (stop.stop_requested()) {
                    return std::unexpected(RobotError::Cancelled);
                }
                auto candidate{ attempt(bad) };
                if (!candidate) {
                    error = candidate.error();
                    break;
                }
                best = *candidate;
                good = bad;
                if (good == bound) {
                    return *best;
                }
                bad = std::min(bound, good * 2);
            }
            for (unsigned n{ 0 }; n < REFINEMENT_STEPS && bad - good > MIN_JOG_INTERVAL; ++n) {
                if (stop.stop_requested()) {
                    return std::unexpected(RobotError::Cancelled);
                }
                const auto middle{ (good + bad) / 2 };
                auto candidate{ attempt(middle) };
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
    RobotError Planner::preparePathJoint(tr::FiniteProgram& out, unsigned joint)
    {
        const auto q0{ RobotModel::joints(m_seed) };
        const auto scales{ m_model->stepsPerJointUnit() };

        const auto motor{ m_model->configuration().motor_for_joint[joint] };
        const auto total{ (m_workspace->pieces[m_pieces - 1].joints[joint].at(1) - q0[joint]) *
                          scales[joint] };
        const auto rounded{ tr::quantize(total) };
        if (!rounded) {
            return rounded.error();
        }
        const auto distance{ *rounded };
        out.displacement[motor] = distance;
        const auto sign{ distance >= 0 ? 1. : -1. };
        auto& path{ out.path[motor] };
        if (distance) {
            path.size = unsigned(m_pieces);
        }
        for (unsigned i{ 0 }; i < m_pieces; ++i) {
            auto& piece{ m_workspace->pieces[i] };
            auto poly{ piece.joints[joint] };
            poly.coefficients[0] -= q0[joint];
            for (auto& c : poly.coefficients) {
                c *= scales[joint];
            }
            if (!distance) {
                if (poly.range().magnitude() > HALF_STEP_TOLERANCE) {
                    return RobotError::Unsupported;
                }
                piece.joints[joint] = { { q0[joint] } };
                continue;
            }
            // One endpoint quantization correction over the whole path,
            // preserving C2 joins. Numerical and grid error stay separate.
            const double correction{ double(distance) - total };
            poly.coefficients[0] += correction * piece.begin;
            poly.coefficients[1] += correction * (piece.end - piece.begin);
            for (unsigned c{ 0 }; c < poly.coefficients.size(); ++c) {
                piece.joints[joint].coefficients[c] = poly.coefficients[c] / scales[joint];
            }
            piece.joints[joint].coefficients[0] += q0[joint];
            for (auto& c : poly.coefficients) {
                c *= sign;
            }
            path.curves[i] = poly;
            path.ends[i] = piece.end;
        }
        if (distance && !path.valid(std::abs(double(distance)))) {
            return RobotError::Unsupported; // reversal: stop at this path boundary
        }
        return RobotError::None;
    }
    std::expected<tr::FiniteProgram, RobotError> Planner::pathProgram(::motion::Profile::Limits factors)
    {
        if (!m_prepared || !m_cartesianPath || !m_pieces || m_pieces > ::motion::ProgressPath::CAPACITY) {
            return std::unexpected(RobotError::Capacity);
        }
        tr::FiniteProgram out{};
        out.kind = tr::ProgramKind::CartesianJog;
        out.limits = m_model->motorLimits();
        for (unsigned joint{}; joint < out.path.size(); ++joint) {
            if (const auto error{ preparePathJoint(out, joint) }; error != RobotError::None) {
                return std::unexpected(error);
            }
        }
        for (unsigned i{ 0 }; i < m_pieces; ++i) {
            unsigned budget{ CERTIFICATION_BUDGET };
            const auto error{ certify(m_workspace->pieces[i].joints, nullptr, 0, 1, 0, budget) };
            if (error != RobotError::None) {
                return std::unexpected(error);
            }
        }
        if (std::ranges::all_of(out.displacement, [](auto d) { return d == 0; })) {
            return std::unexpected(RobotError::PositionLimit);
        }
        auto limits{ scalarLimits(1) };
        double stretch{ 1 };
        for (unsigned i{ 0 }; i < 3; ++i) {
            auto& physical{ out.limits[i] };
            physical.acceleration *= factors.acceleration;
            physical.deceleration *= factors.deceleration;
            physical.jerk *= factors.jerk;
            if (!out.path[i].size) {
                continue;
            }
            const auto bounds{ out.path[i].dynamicBounds(limits) };
            stretch =
              std::max({ stretch,
                         bounds[0] / physical.velocity,
                         std::sqrt(bounds[1] / std::min(physical.acceleration, physical.deceleration)),
                         std::cbrt(bounds[2] / physical.jerk) });
        }
        stretch *= RETIMING_MARGIN;
        limits.velocity /= stretch;
        limits.acceleration /= stretch * stretch;
        limits.deceleration /= stretch * stretch;
        limits.jerk /= stretch * stretch * stretch;
        const auto law{ ::motion::Profile::create(1, limits) };
        if (!law || law->duration() > MAX_JOG_SECONDS) {
            return std::unexpected(RobotError::Timing);
        }
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
        unsigned axis{ 3 };
        for (unsigned i{ 0 }; i < 3; ++i) {
            if (!std::isfinite(direction[i]) || (direction[i] != 0 && std::abs(direction[i]) != 1)) {
                return std::unexpected(RobotError::InvalidInput);
            }
            if (direction[i] != 0) {
                if (axis != 3) {
                    return std::unexpected(RobotError::InvalidInput);
                }
                axis = i;
            }
        }
        for (const auto factor : { speed, acceleration, deceleration, jerk }) {
            if (!std::isfinite(factor) || factor <= 0 || factor > 1) {
                return std::unexpected(RobotError::InvalidInput);
            }
        }
        if (axis == 3) {
            return std::unexpected(RobotError::InvalidInput);
        }
        tr::Positions positions{};
        for (unsigned i{ 0 }; i < 3; ++i) {
            positions[i] = static_cast<double>(counts[i]);
        }
        const auto fk{ m_model->kinematics().forward(m_model->toJoints(positions)) };
        if (!fk) {
            return std::unexpected(RobotError::Unreachable);
        }
        const auto from{ fk->position };
        const auto xyz{ [](CartesianPosition p) { return std::array{ p.x.get(), p.y.get(), p.z.get() }; } };
        const auto begin{ xyz(from) };
        const auto edge{ xyz(direction[axis] > 0 ? m_model->configuration().workspace_maximum
                                                 : m_model->configuration().workspace_minimum) };
        const auto bound{ direction[axis] * (edge[axis] - begin[axis]) };
        if (!std::isfinite(bound) || bound <= 0) {
            return std::unexpected(RobotError::Workspace);
        }
        auto attempt{ [&](double length) -> std::expected<tr::FiniteProgram, RobotError> {
            auto end{ begin };
            end[axis] += direction[axis] * length;
            Move move{ .kind = MoveKind::Linear,
                       .target = { pnm::units::Distance::create(end[0]),
                                   pnm::units::Distance::create(end[1]),
                                   pnm::units::Distance::create(end[2]) },
                       .speed = speed };
            const auto error{ prepare(move, counts) };
            if (error != RobotError::None) {
                return std::unexpected(error);
            }
            return pathProgram({ .acceleration = acceleration, .deceleration = deceleration, .jerk = jerk });
        } };
        return find_jog_interval(bound, m_stop, attempt);
    }
}
