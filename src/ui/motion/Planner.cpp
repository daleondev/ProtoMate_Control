#include "Planner.hpp"
#include "profiling/Trace.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace ui::motion
{
    using namespace pnm::units;
    namespace tr = ::motion::trajectory;
    using P = ::motion::Polynomial;
    using I = ::motion::Interval;
    namespace
    {
        constexpr double BOUNDARY_POSITION_EPS{ 1e-8 };
        constexpr unsigned CERTIFICATION_VISIT_LIMIT{ 8192 };
        constexpr unsigned CERTIFICATION_WORK_SIZE{ 18 };
        constexpr unsigned CERTIFICATION_DEPTH_LIMIT{ 16 };
        constexpr double MIN_PARAMETER_INTERVAL{ 1e-9 };
        constexpr double INITIAL_SCALAR_LIMIT{ 1e6 };
        constexpr double PROGRESS_EPS{ 1e-12 };
        constexpr double DYNAMIC_MARGIN_FACTOR{ 1.25 };
        constexpr double FINITE_DURATION_LIMIT_S{ 90. };
        constexpr unsigned COMPILATION_WORK_SIZE{ 22 };
        constexpr unsigned COMPILATION_DEPTH_LIMIT{ 20 };
        constexpr unsigned MIN_COMPILATION_TICKS{ 10 };
        constexpr unsigned COMPILED_DURATION_LIMIT_S{ 3600 };
        constexpr unsigned MAX_BOUNDARIES{ 160 };
        constexpr double PI{ std::numbers::pi };
        constexpr double HALF{ .5 }, PATH_ERROR_FRACTION{ .45 };
        constexpr double GEOMETRY_EPS{ 1e-11 };
        I add(I a, I b) { return { a.minimum + b.minimum, a.maximum + b.maximum }; }
        I scale(I a, double b)
        {
            return b >= 0 ? I{ a.minimum * b, a.maximum * b } : I{ a.maximum * b, a.minimum * b };
        }
        I multiply(I a, I b)
        {
            const std::array v{
                a.minimum * b.minimum, a.minimum * b.maximum, a.maximum * b.minimum, a.maximum * b.maximum
            };
            return { *std::ranges::min_element(v), *std::ranges::max_element(v) };
        }
        I shift(I a, double b) { return { a.minimum + b, a.maximum + b }; }
        I sine(I a)
        {
            if (!a.finite() || a.maximum - a.minimum >= 2 * PI) {
                return { -1, 1 };
            }
            I out{ std::min(std::sin(a.minimum), std::sin(a.maximum)),
                   std::max(std::sin(a.minimum), std::sin(a.maximum)) };
            if (std::ceil((a.minimum - PI / 2) / (2 * PI)) <= std::floor((a.maximum - PI / 2) / (2 * PI))) {
                out.maximum = 1;
            }
            if (std::ceil((a.minimum + PI / 2) / (2 * PI)) <= std::floor((a.maximum + PI / 2) / (2 * PI))) {
                out.minimum = -1;
            }
            out.minimum -= GEOMETRY_EPS;
            out.maximum += GEOMETRY_EPS;
            return out;
        }
        I cosine(I a) { return sine(shift(a, PI / 2)); }
        std::array<double, 3> xyz(CartesianPosition p) { return { p.x.get(), p.y.get(), p.z.get() }; }
        CartesianPosition point(const std::array<double, 3>& p)
        {
            return { Distance::create(p[0]), Distance::create(p[1]), Distance::create(p[2]) };
        }
        RobotError kinematic_error(ScaraKinematics::Error error)
        {
            using E = ScaraKinematics::Error;
            switch (error) {
                case E::Unreachable:
                    return RobotError::Unreachable;
                case E::JointLimitExceeded:
                    return RobotError::PositionLimit;
                case E::Singularity:
                    return RobotError::Singularity;
                default:
                    return RobotError::InvalidInput;
            }
        }
        std::array<double, 3> components(ScaraKinematics::JointVelocity v)
        {
            return { v.shoulder.get(), v.elbow.get(), v.z.get() };
        }
        double deviation_bound(const std::array<P, 3>& desired,
                               const std::array<I, 3>& derivatives,
                               const std::array<double, 3>& actual,
                               const std::array<I, 2>& angles,
                               const std::array<double, 2>& lengths,
                               I interval)
        {
            const auto a{ angles[0] };
            const auto b{ angles[1] };
            const auto l1{ lengths[0] };
            const auto l2{ lengths[1] };
            const auto from{ interval.minimum };
            const auto to{ interval.maximum };
            const auto middle{ (from + to) * HALF };
            const auto joint_sum{ add(derivatives[0], derivatives[1]) };
            const std::array<I, 3> velocity{ add(scale(multiply(sine(a), derivatives[0]), -l1),
                                                 scale(multiply(sine(b), joint_sum), -l2)),
                                             add(scale(multiply(cosine(a), derivatives[0]), l1),
                                                 scale(multiply(cosine(b), joint_sum), l2)),
                                             derivatives[2] };
            double deviation2{ 0 };
            for (unsigned i{ 0 }; i < 3; ++i) {
                const auto expected{ desired[i].range(from, to, 1) };
                const auto difference{ add(velocity[i], scale(expected, -1)) };
                const double bound{ std::abs(actual[i] - desired[i].at(middle)) +
                                    (difference.magnitude() * (to - from) * HALF) + GEOMETRY_EPS };
                deviation2 += bound * bound;
            }
            return std::sqrt(deviation2);
        }
    }

    Planner::Planner(const RobotModel& model, Workspace& workspace, std::stop_token stop)
      : m_model{ &model }
      , m_stop{ std::move(stop) }
      , m_workspace{ &workspace }
    {
    }

    RobotError Planner::prepareCartesian(const std::array<double, 3>& q0)
    {
        const auto first{ inverseAt(0) };
        if (!first) {
            return first.error();
        }
        for (unsigned i{ 0 }; i < 3; ++i) {
            if (std::abs((*first)[i].position - q0[i]) > BOUNDARY_POSITION_EPS) {
                return RobotError::Discontinuity;
            }
        }
        const auto error{ build(0, 1, 0) };
        if (error != RobotError::None) {
            return error;
        }
        m_pathDistance = 0;
        for (const auto& p : m_cartesian) {
            const double bound{ p.range(0, 1, 1).magnitude() };
            m_pathDistance += bound * bound;
        }
        m_pathDistance = std::sqrt(m_pathDistance);
        return RobotError::None;
    }
    RobotError Planner::prepareJoints(JointPosition target, const std::array<double, 3>& q0)
    {
        auto& piece{ m_workspace->pieces[0] };
        piece.begin = 0;
        piece.end = 1;
        if (m_move.kind == MoveKind::JointCurve) {
            piece.joints = m_move.curve;
            for (unsigned i{ 0 }; i < 3; ++i) {
                if (std::abs(piece.joints[i].at(0) - q0[i]) > BOUNDARY_POSITION_EPS) {
                    return RobotError::Discontinuity;
                }
            }
        }
        else {
            tr::Positions motor{};
            try {
                motor = m_model->toMotors(target);
            } catch (...) {
                return RobotError::InvalidInput;
            }
            for (auto& p : motor) {
                const auto rounded{ tr::quantize(p) };
                if (!rounded) {
                    return rounded.error();
                }
                p = static_cast<double>(*rounded);
            }
            const auto q1{ RobotModel::joints(m_model->toJoints(motor)) };
            for (unsigned i{ 0 }; i < 3; ++i) {
                piece.joints[i] = { { q0[i], q1[i] - q0[i] } };
            }
        }
        unsigned budget{ CERTIFICATION_VISIT_LIMIT };
        const auto error{ certify(piece.joints, nullptr, 0, 1, 0, budget) };
        if (error != RobotError::None) {
            return error;
        }
        m_pieces = 1;
        m_pathDistance = 0;
        return RobotError::None;
    }
    RobotError Planner::prepare(const Move& move, const tr::Counts& counts)
    {
        profiling::Scope measurement{ PROFILE_SITE_PLANNER, PROFILE_CAT_MOTION };
        m_prepared = false;
        if (m_stop.stop_requested()) {
            return RobotError::Cancelled;
        }
        m_pieces = 0;
        m_segments = 0;
        m_move = move;
        m_start = counts;
        if (!std::isfinite(move.speed) || move.speed <= 0 || move.speed > 1 || !move.feed.isFinite() ||
            move.feed.get() < 0 || !move.blend.isFinite() || move.blend.get() < 0 ||
            move.kind > MoveKind::CartesianCurve || move.frame > CoordinateFrame::Work) {
            return RobotError::InvalidInput;
        }
        tr::Positions start{};
        for (unsigned i{ 0 }; i < 3; ++i) {
            start[i] = static_cast<double>(counts[i]);
        }
        try {
            m_seed = m_model->toJoints(start);
        } catch (...) {
            return RobotError::InvalidInput;
        }
        const auto branch{ m_model->kinematics().elbowBranch(m_seed) };
        if (!branch) {
            return kinematic_error(branch.error());
        }
        m_branch = *branch;
        const auto from{ m_model->kinematics().forward(m_seed) };
        if (!from) {
            return kinematic_error(from.error());
        }
        const auto q0{ RobotModel::joints(m_seed) };
        JointPosition target{ move.joints };
        m_cartesianPath = move.kind == MoveKind::Linear || move.kind == MoveKind::CartesianCurve;
        if (move.kind == MoveKind::Point || move.kind == MoveKind::Linear) {
            const auto endpoint{ move.frame == CoordinateFrame::Work ? m_model->fromWork(move.target)
                                                                     : move.target };
            const auto solution{ m_model->kinematics().inverse(endpoint, m_seed, m_branch) };
            if (!solution) {
                return kinematic_error(solution.error());
            }
            target = *solution;
            const auto a{ xyz(from->position) };
            const auto b{ xyz(endpoint) };
            for (unsigned i{ 0 }; i < 3; ++i) {
                m_cartesian[i] = { { a[i], b[i] - a[i] } };
            }
        }
        else if (move.kind == MoveKind::CartesianCurve) {
            m_cartesian = move.curve;
        }
        const auto error{ m_cartesianPath ? prepareCartesian(q0) : prepareJoints(target, q0) };
        if (error != RobotError::None) {
            return error;
        }

        m_prepared = true;
        return RobotError::None;
    }

    std::expected<Planner::JointState, RobotError> Planner::inverseAt(double s) const
    {
        std::array<double, 3> p{};
        std::array<double, 3> v{};
        std::array<double, 3> a{};
        for (unsigned i{ 0 }; i < 3; ++i) {
            p[i] = m_cartesian[i].at(s);
            v[i] = m_cartesian[i].at(s, 1);
            a[i] = m_cartesian[i].at(s, 2);
        }
        const auto q{ m_model->kinematics().inverse(point(p), m_seed, m_branch) };
        if (!q) {
            return std::unexpected(kinematic_error(q.error()));
        }
        const auto velocity{ m_model->kinematics().inverseVelocity(
          *q, { .x = Velocity::create(v[0]), .y = Velocity::create(v[1]), .z = Velocity::create(v[2]) }) };
        if (!velocity) {
            return std::unexpected(kinematic_error(velocity.error()));
        }
        const auto pos{ RobotModel::joints(*q) };
        const auto speed{ components(*velocity) };
        const auto& g{ m_model->configuration().geometry };
        const double first{ g.base_yaw.get() + pos[0] };
        const double phase{ std::atan2(g.tool_offset.y.get(),
                                       (g.second_arm_length + g.tool_offset.x).get()) };
        const double second{ first + pos[1] + phase };
        const double l1{ g.first_arm_length.get() };
        const double l2{ std::hypot((g.second_arm_length + g.tool_offset.x).get(), g.tool_offset.y.get()) };
        const double w1{ speed[0] * speed[0] };
        const double w2{ (speed[0] + speed[1]) * (speed[0] + speed[1]) };
        // J*q'' = p'' - Jdot*q'. inverseVelocity applies the same inverse J.
        const auto acceleration{ m_model->kinematics().inverseVelocity(
          *q,
          { .x = Velocity::create(a[0] + (l1 * std::cos(first) * w1) + (l2 * std::cos(second) * w2)),
            .y = Velocity::create(a[1] + (l1 * std::sin(first) * w1) + (l2 * std::sin(second) * w2)),
            .z = Velocity::create(a[2]) }) };
        if (!acceleration) {
            return std::unexpected(kinematic_error(acceleration.error()));
        }
        const auto acc{ components(*acceleration) };
        JointState result{};
        for (unsigned i{ 0 }; i < 3; ++i) {
            result[i] = { .position = pos[i], .velocity = speed[i], .acceleration = acc[i] };
        }
        return result;
    }

    RobotError Planner::build(double from, double to, unsigned depth)
    {
        struct Interval
        {
            double from, to;
            unsigned depth;
        };
        std::array<Interval, CERTIFICATION_WORK_SIZE> work{};
        work[0] = { .from = from, .to = to, .depth = depth };
        std::size_t pending{ 1 };
        unsigned visits{ 0 };
        while (pending) {
            if (m_stop.stop_requested()) {
                return RobotError::Cancelled;
            }
            if (++visits > CERTIFICATION_VISIT_LIMIT) {
                return RobotError::SubdivisionLimit;
            }
            const auto interval{ work[--pending] };
            from = interval.from;
            to = interval.to;
            depth = interval.depth;
            const auto a{ inverseAt(from) };
            const auto b{ inverseAt(to) };
            if (!a) {
                return a.error();
            }
            if (!b) {
                return b.error();
            }
            Piece piece{ .begin = from, .end = to };
            std::array<P, 3> desired{};
            for (unsigned i{ 0 }; i < 3; ++i) {
                piece.joints[i] = ::motion::hermite((*a)[i], (*b)[i], to - from);
                desired[i] = m_cartesian[i].restrict(from, to);
            }
            unsigned budget{ CERTIFICATION_VISIT_LIMIT };
            const auto error{ certify(piece.joints, &desired, 0, 1, 0, budget) };
            if (error == RobotError::None) {
                if (m_pieces == m_workspace->pieces.size()) {
                    return RobotError::Capacity;
                }
                m_workspace->pieces[m_pieces++] = piece;
                continue;
            }
            if (depth >= CERTIFICATION_DEPTH_LIMIT) {
                return error;
            }
            const double middle{ (from + to) * HALF };
            const auto mid{ inverseAt(middle) };
            if (!mid) {
                return mid.error();
            }
            work[pending++] = { .from = middle, .to = to, .depth = depth + 1 };
            work[pending++] = { .from = from, .to = middle, .depth = depth + 1 };
        }
        return RobotError::None;
    }

    std::expected<Planner::Certification, RobotError> Planner::certifyRange(const std::array<P, 3>& q,
                                                                            const std::array<P, 3>* desired,
                                                                            I interval) const
    {
        const auto from{ interval.minimum };
        const auto to{ interval.maximum };
        const double middle{ (from + to) * HALF };
        std::array<double, 3> position{};
        std::array<I, 3> ranges{};
        std::array<I, 3> derivatives{};
        const auto limits{ m_model->jointLimits() };
        bool proven{ true };
        RobotError reason{ RobotError::PositionLimit };
        for (unsigned i{ 0 }; i < 3; ++i) {
            if (!q[i].finite()) {
                return std::unexpected(RobotError::InvalidInput);
            }
            position[i] = q[i].at(middle);
            ranges[i] = q[i].range(from, to);
            derivatives[i] = q[i].range(from, to, 1);
            if (position[i] < limits[i].minimum - GEOMETRY_EPS ||
                position[i] > limits[i].maximum + GEOMETRY_EPS) {
                return std::unexpected(RobotError::PositionLimit);
            }
            proven &= ranges[i].minimum >= limits[i].minimum - GEOMETRY_EPS &&
                      ranges[i].maximum <= limits[i].maximum + GEOMETRY_EPS;
        }
        const auto& g{ m_model->configuration().geometry };
        const double phase{ std::atan2(g.tool_offset.y.get(),
                                       (g.second_arm_length + g.tool_offset.x).get()) };
        const auto bend{ sine(shift(ranges[1], phase)) };
        const double sign{ m_branch == ScaraKinematics::ElbowBranch::Positive ? 1.0 : -1.0 };
        if (sign * std::sin(position[1] + phase) <= g.minimum_bend_sine) {
            return std::unexpected(RobotError::Singularity);
        }
        const bool bend_valid{ sign > 0 ? bend.minimum > g.minimum_bend_sine
                                        : bend.maximum < -g.minimum_bend_sine };
        if (!bend_valid) {
            proven = false;
            reason = RobotError::Singularity;
        }
        const auto a{ shift(ranges[0], g.base_yaw.get()) };
        const auto b{ shift(add(ranges[0], ranges[1]), g.base_yaw.get() + phase) };
        const double l1{ g.first_arm_length.get() };
        const double l2{ std::hypot((g.second_arm_length + g.tool_offset.x).get(), g.tool_offset.y.get()) };
        std::array<I, 3> tcp{ shift(add(scale(cosine(a), l1), scale(cosine(b), l2)), g.base_origin.x.get()),
                              shift(add(scale(sine(a), l1), scale(sine(b), l2)), g.base_origin.y.get()),
                              shift(ranges[2], (g.base_origin.z + g.tool_offset.z).get()) };
        const auto fk{ m_model->kinematics().forward(RobotModel::joints(position)) };
        if (!fk) {
            return std::unexpected(kinematic_error(fk.error()));
        }
        const auto actual{ xyz(fk->position) };
        const auto lo{ xyz(m_model->configuration().workspace_minimum) };
        const auto hi{ xyz(m_model->configuration().workspace_maximum) };
        for (unsigned i{ 0 }; i < 3; ++i) {
            if (actual[i] < lo[i] - GEOMETRY_EPS || actual[i] > hi[i] + GEOMETRY_EPS) {
                return std::unexpected(RobotError::Workspace);
            }
            if (tcp[i].minimum < lo[i] - GEOMETRY_EPS || tcp[i].maximum > hi[i] + GEOMETRY_EPS) {
                proven = false;
                reason = RobotError::Workspace;
            }
        }
        if (desired) {
            const auto deviation{ deviation_bound(
              *desired, derivatives, actual, { a, b }, { l1, l2 }, interval) };
            const double available{ m_model->configuration().path_tolerance.get() * PATH_ERROR_FRACTION };
            if (deviation > available) {
                proven = false;
                reason = RobotError::PathTolerance;
            }
        }
        return Certification{ .proven = proven, .reason = reason };
    }
    RobotError Planner::certify(const std::array<P, 3>& q,
                                const std::array<P, 3>* desired,
                                double from,
                                double to,
                                unsigned depth,
                                unsigned& budget) const
    {
        struct Interval
        {
            double from, to;
            unsigned depth;
        };
        std::array<Interval, CERTIFICATION_WORK_SIZE> work{};
        work[0] = { .from = from, .to = to, .depth = depth };
        std::size_t pending{ 1 };
        while (pending) {
            if (m_stop.stop_requested()) {
                return RobotError::Cancelled;
            }
            const auto interval{ work[--pending] };
            from = interval.from;
            to = interval.to;
            depth = interval.depth;
            if (budget == 0) {
                return RobotError::SubdivisionLimit;
            }
            --budget;
            const auto checked{ certifyRange(q, desired, { from, to }) };
            if (!checked) {
                return checked.error();
            }
            const auto proven{ checked->proven };
            const auto reason{ checked->reason };
            const double middle{ (from + to) * HALF };
            if (proven) {
                continue;
            }
            if (depth >= CERTIFICATION_DEPTH_LIMIT || to - from < MIN_PARAMETER_INTERVAL) {
                return reason;
            }
            work[pending++] = { .from = middle, .to = to, .depth = depth + 1 };
            work[pending++] = { .from = from, .to = middle, .depth = depth + 1 };
        }
        return RobotError::None;
    }

    ::motion::Profile::Limits Planner::scalarLimits(double speed) const
    {
        const auto limits{ m_model->jointLimits() };
        std::array<double, 3> d1{};
        std::array<double, 3> d2{};
        std::array<double, 3> d3{};
        for (std::size_t n{ 0 }; n < m_pieces; ++n) {
            const auto& p{ m_workspace->pieces[n] };
            const double w{ p.end - p.begin };
            for (unsigned i{ 0 }; i < 3; ++i) {
                d1[i] = std::max(d1[i], p.joints[i].range(0, 1, 1).magnitude() / w);
                d2[i] = std::max(d2[i], p.joints[i].range(0, 1, 2).magnitude() / (w * w));
                d3[i] = std::max(d3[i], p.joints[i].range(0, 1, 3).magnitude() / (w * w * w));
            }
        }
        ::motion::Profile::Limits scalar{ .velocity = INITIAL_SCALAR_LIMIT,
                                          .acceleration = INITIAL_SCALAR_LIMIT,
                                          .deceleration = INITIAL_SCALAR_LIMIT,
                                          .jerk = INITIAL_SCALAR_LIMIT };
        for (unsigned i{ 0 }; i < 3; ++i) {
            if (d1[i] < PROGRESS_EPS) {
                continue;
            }
            scalar.velocity = std::min(scalar.velocity, limits[i].velocity / d1[i]);
            scalar.acceleration = std::min(scalar.acceleration, limits[i].acceleration / d1[i]);
            scalar.deceleration = std::min(scalar.deceleration, limits[i].deceleration / d1[i]);
            scalar.jerk = std::min(scalar.jerk, limits[i].jerk / d1[i]);
        }
        if (m_cartesianPath && m_pathDistance > 0) {
            const double feed{ m_move.feed.get() > 0
                                 ? std::min(m_move.feed.get(), m_model->configuration().feed.get())
                                 : m_model->configuration().feed.get() };
            scalar.velocity = std::min(scalar.velocity, feed / m_pathDistance);
        }
        scalar.velocity *= m_move.speed * speed;
        double stretch{ 1 };
        const double v{ scalar.velocity };
        const double a{ std::max(scalar.acceleration, scalar.deceleration) };
        const double j{ scalar.jerk };
        for (unsigned i{ 0 }; i < 3; ++i) {
            stretch = std::max(stretch, d1[i] * v / limits[i].velocity);
            stretch = std::max(stretch,
                               std::sqrt((d2[i] * v * v + d1[i] * a) /
                                         std::min(limits[i].acceleration, limits[i].deceleration)));
            stretch = std::max(
              stretch, std::cbrt((d3[i] * v * v * v + 3 * d2[i] * v * a + d1[i] * j) / limits[i].jerk));
        }
        // Reserve margin for tick quantization and the time-domain interpolation.
        stretch *= DYNAMIC_MARGIN_FACTOR;
        scalar.velocity /= stretch;
        scalar.acceleration /= stretch * stretch;
        scalar.deceleration /= stretch * stretch;
        scalar.jerk /= stretch * stretch * stretch;
        return scalar;
    }

    std::expected<tr::FiniteProgram, RobotError> Planner::finite(double acceleration,
                                                                 double deceleration,
                                                                 double jerk,
                                                                 double* estimated_seconds) const
    {
        if (estimated_seconds) {
            *estimated_seconds = 0;
        }
        if (!m_prepared || (m_move.kind != MoveKind::Joints && m_move.kind != MoveKind::Point) ||
            m_pieces != 1) {
            return std::unexpected(RobotError::Unsupported);
        }
        for (auto factor : { acceleration, deceleration, jerk }) {
            if (!std::isfinite(factor) || factor <= 0 || factor > 1) {
                return std::unexpected(RobotError::InvalidInput);
            }
        }
        tr::FiniteProgram out{};
        out.limits = m_model->motorLimits();
        std::array<double, 3> target{};
        for (unsigned i{ 0 }; i < 3; ++i) {
            target[i] = m_workspace->pieces[0].joints[i].at(1);
        }
        const auto motors{ m_model->toMotors(RobotModel::joints(target)) };
        bool moving{};
        for (unsigned i{ 0 }; i < 3; ++i) {
            const auto count{ tr::quantize(motors[i]) };
            if (!count) {
                return std::unexpected(count.error());
            }
            out.displacement[i] = *count - m_start[i];
            moving |= out.displacement[i] != 0;
        }
        if (!moving) {
            return out;
        }
        tr::Positions first{};
        tr::Positions last{};
        for (unsigned i{ 0 }; i < 3; ++i) {
            first[i] = static_cast<double>(m_start[i]);
            last[i] = first[i] + static_cast<double>(out.displacement[i]);
        }
        const auto qa{ RobotModel::joints(m_model->toJoints(first)) };
        const auto qb{ RobotModel::joints(m_model->toJoints(last)) };
        std::array<P, 3> quantized_path{};
        for (unsigned i{ 0 }; i < 3; ++i) {
            quantized_path[i].coefficients = { qa[i], qb[i] - qa[i] };
        }
        unsigned certification_budget{ CERTIFICATION_VISIT_LIMIT };
        if (const auto error{ certify(quantized_path, nullptr, 0, 1, 0, certification_budget) };
            error != RobotError::None) {
            return std::unexpected(error);
        }
        // Exact linear joint interpolation needs no sampled/quintic expansion.
        // Every axis follows this one scalar law and its one stopping branch.
        auto limits{ scalarLimits(1) };
        // Rounding a short displacement upward must not increase its physical
        // dynamics above the axis limits used for the unquantized plan.
        for (unsigned i{ 0 }; i < 3; ++i) {
            if (const auto d{ std::abs(double(out.displacement[i])) }; d > 0) {
                limits.velocity = std::min(limits.velocity, out.limits[i].velocity * m_move.speed / d);
                limits.acceleration = std::min(limits.acceleration, out.limits[i].acceleration / d);
                limits.deceleration = std::min(limits.deceleration, out.limits[i].deceleration / d);
                limits.jerk = std::min(limits.jerk, out.limits[i].jerk / d);
            }
        }
        limits.acceleration *= acceleration;
        limits.deceleration *= deceleration;
        limits.jerk *= jerk;
        const auto law{ ::motion::Profile::create(1., limits) };
        if (law && estimated_seconds) {
            *estimated_seconds = law->duration();
        }
        if (!law || law->duration() > FINITE_DURATION_LIMIT_S) {
            return std::unexpected(RobotError::Timing);
        }
        out.progress = law->description();
        out.progress_limits = limits;
        return out;
    }

    Planner::JointState Planner::jointAt(PathState state) const
    {
        const auto [s, velocity, acceleration]{ state };
        std::size_t index{ 0 };
        while (index + 1 < m_pieces && s > m_workspace->pieces[index].end) {
            ++index;
        }
        const auto& p{ m_workspace->pieces[index] };
        const double w{ p.end - p.begin };
        const double u{ std::clamp((s - p.begin) / w, 0.0, 1.0) };
        JointState out{};
        for (unsigned i{ 0 }; i < 3; ++i) {
            const double d1{ p.joints[i].at(u, 1) / w };
            const double d2{ p.joints[i].at(u, 2) / (w * w) };
            out[i] = { .position = p.joints[i].at(u),
                       .velocity = d1 * velocity,
                       .acceleration = (d2 * velocity * velocity) + (d1 * acceleration) };
        }
        return out;
    }
    PathState Planner::pathAt(double seconds) const noexcept
    {
        const auto s{ m_profile.at(seconds) };
        return { .position = m_offset + s.position, .velocity = s.velocity, .acceleration = s.acceleration };
    }
    RobotError Planner::validatePath(const tr::Segment& segment, const std::array<P, 3>* reference) const
    {
        if (m_stop.stop_requested()) {
            return RobotError::Cancelled;
        }
        const auto error{ tr::validate(segment, m_model->motorLimits()) };
        if (error != RobotError::None) {
            return error;
        }
        const auto zero{ m_model->toMotors(RobotModel::joints(std::array<double, 3>{})) };
        const auto scale{ m_model->stepsPerJointUnit() };
        std::array<P, 3> joints{};
        for (unsigned j{ 0 }; j < 3; ++j) {
            const auto motor{ m_model->configuration().motor_for_joint[j] };
            joints[j] = segment.motors[motor];
            joints[j].coefficients[0] -= zero[motor];
            for (auto& c : joints[j].coefficients) {
                c /= scale[j];
            }
        }
        unsigned budget{ CERTIFICATION_VISIT_LIMIT };
        return certify(joints, reference, 0, 1, 0, budget);
    }

    std::expected<tr::Segment, RobotError> Planner::makeSegment(TickRange ticks,
                                                                PathState a,
                                                                PathState b) const
    {
        auto start{ m_model->toMotors(jointAt(a)) };
        auto end{ m_model->toMotors(jointAt(b)) };
        if (ticks.to == m_durationTicks) {
            for (auto& s : end) {
                const auto rounded{ tr::quantize(s.position) };
                if (!rounded) {
                    return std::unexpected(rounded.error());
                }
                s.position = static_cast<double>(*rounded);
            }
        }
        tr::Segment segment{ .ticks = ticks.to - ticks.from,
                             .job = m_move.job,
                             .path_begin = a.position,
                             .path_end = b.position };
        const double duration{ static_cast<double>(segment.ticks) * tr::TICK_SECONDS };
        for (unsigned i{ 0 }; i < 3; ++i) {
            segment.motors[i] = ::motion::hermite(start[i], end[i], duration);
        }
        return segment;
    }
    std::array<P, 3> Planner::timeReference(PathState a, PathState b, double duration) const
    {
        std::array<P, 3> reference{};
        if (m_cartesianPath) {
            // Fit the exact requested Cartesian curve in time using the same
            // endpoint derivatives. For MoveLinear this is the scalar time law.
            for (unsigned i{ 0 }; i < 3; ++i) {
                const auto& p{ m_cartesian[i] };
                const ::motion::KinematicState p0{ .position = p.at(a.position),
                                                   .velocity = p.at(a.position, 1) * a.velocity,
                                                   .acceleration =
                                                     (p.at(a.position, 2) * a.velocity * a.velocity) +
                                                     (p.at(a.position, 1) * a.acceleration) };
                const ::motion::KinematicState p1{ .position = p.at(b.position),
                                                   .velocity = p.at(b.position, 1) * b.velocity,
                                                   .acceleration =
                                                     (p.at(b.position, 2) * b.velocity * b.velocity) +
                                                     (p.at(b.position, 1) * b.acceleration) };
                reference[i] = ::motion::hermite(p0, p1, duration);
            }
        }
        return reference;
    }
    RobotError Planner::appendSegment(const tr::Segment& segment)
    {
        if (m_segments == m_workspace->segments.size()) {
            return RobotError::Capacity;
        }
        if (m_segments && !tr::continuous(m_workspace->segments[m_segments - 1].at(1), segment.at(0))) {
            return RobotError::Discontinuity;
        }
        m_workspace->segments[m_segments++] = segment;
        return RobotError::None;
    }
    RobotError Planner::compileInterval(std::uint64_t from, std::uint64_t to, unsigned depth)
    {
        struct Interval
        {
            std::uint64_t from, to;
            unsigned depth;
        };
        std::array<Interval, COMPILATION_WORK_SIZE> work{};
        work[0] = { .from = from, .to = to, .depth = depth };
        std::size_t pending{ 1 };
        unsigned visits{ 0 };
        while (pending) {
            if (m_stop.stop_requested()) {
                return RobotError::Cancelled;
            }
            if (++visits > CERTIFICATION_VISIT_LIMIT) {
                return RobotError::SubdivisionLimit;
            }
            const auto interval{ work[--pending] };
            from = interval.from;
            to = interval.to;
            depth = interval.depth;
            if (to <= from) {
                continue;
            }
            const auto a{ pathAt(static_cast<double>(from) * tr::TICK_SECONDS) };
            const auto b{ pathAt(static_cast<double>(to) * tr::TICK_SECONDS) };
            auto candidate{ makeSegment({ .from = from, .to = to }, a, b) };
            if (!candidate) {
                return candidate.error();
            }
            const auto& segment{ *candidate };
            const double duration{ static_cast<double>(segment.ticks) * tr::TICK_SECONDS };
            const auto reference{ timeReference(a, b, duration) };
            auto error{ validatePath(segment, m_cartesianPath ? &reference : nullptr) };
            if (error == RobotError::None && m_cartesianPath) {
                unsigned budget{ CERTIFICATION_VISIT_LIMIT };
                error = certifyReference(
                  reference, static_cast<double>(from) * tr::TICK_SECONDS, duration, 0, 1, 0, budget);
            }
            if (error == RobotError::None) {
                const auto appended{ appendSegment(segment) };
                if (appended != RobotError::None) {
                    return appended;
                }
                continue;
            }
            if (depth >= COMPILATION_DEPTH_LIMIT || to - from <= MIN_COMPILATION_TICKS) {
                return error;
            }
            const auto middle{ from + ((to - from) / 2) };
            work[pending++] = { .from = middle, .to = to, .depth = depth + 1 };
            work[pending++] = { .from = from, .to = middle, .depth = depth + 1 };
        }
        return RobotError::None;
    }

    RobotError Planner::certifyReference(const std::array<P, 3>& reference,
                                         double start,
                                         double duration,
                                         double from,
                                         double to,
                                         unsigned depth,
                                         unsigned& budget) const
    {
        struct Interval
        {
            double from, to;
            unsigned depth;
        };
        std::array<Interval, CERTIFICATION_WORK_SIZE> work{};
        work[0] = { .from = from, .to = to, .depth = depth };
        std::size_t pending{ 1 };
        while (pending) {
            if (m_stop.stop_requested()) {
                return RobotError::Cancelled;
            }
            const auto interval{ work[--pending] };
            from = interval.from;
            to = interval.to;
            depth = interval.depth;
            if (!budget) {
                return RobotError::SubdivisionLimit;
            }
            --budget;
            const double t0{ start + (duration * from) };
            const double t1{ start + (duration * to) };
            const double middle{ (from + to) * HALF };
            const auto a{ pathAt(t0) };
            const auto b{ pathAt(t1) };
            const auto mid{ pathAt(start + (duration * middle)) };
            // Bound the actual scalar velocity on every intersected constant-jerk
            // phase. This also covers intervals straddling tick-rounded phase ends.
            I speed{ std::min(a.velocity, b.velocity), std::max(a.velocity, b.velocity) };
            double phase_start{ 0 };
            const auto description{ m_profile.description() };
            for (unsigned i{ 0 }; i < description.size; ++i) {
                const auto& phase{ description.phases[i] };
                const double lo{ std::max(t0 - phase_start, 0.0) };
                const double hi{ std::min(t1 - phase_start, phase.duration) };
                if (lo <= hi) {
                    const auto v{ P{ { phase.velocity, phase.acceleration, phase.jerk * HALF } }.range(lo,
                                                                                                       hi) };
                    speed.minimum = std::min(speed.minimum, v.minimum);
                    speed.maximum = std::max(speed.maximum, v.maximum);
                }
                phase_start += phase.duration;
            }
            double squared{ 0 };
            for (unsigned i{ 0 }; i < 3; ++i) {
                const auto exact{ multiply(m_cartesian[i].range(a.position, b.position, 1),
                                           scale(speed, duration)) };
                const auto derivative{ add(exact, scale(reference[i].range(from, to, 1), -1)) };
                const double bound{ std::abs(m_cartesian[i].at(mid.position) - reference[i].at(middle)) +
                                    (derivative.magnitude() * (to - from) * HALF) + GEOMETRY_EPS };
                squared += bound * bound;
            }
            const double available{ m_model->configuration().path_tolerance.get() * PATH_ERROR_FRACTION };
            if (std::sqrt(squared) <= available) {
                continue;
            }
            if (depth >= CERTIFICATION_DEPTH_LIMIT) {
                return RobotError::SubdivisionLimit;
            }
            work[pending++] = { .from = middle, .to = to, .depth = depth + 1 };
            work[pending++] = { .from = from, .to = middle, .depth = depth + 1 };
        }
        return RobotError::None;
    }

    std::expected<PlanSummary, RobotError> Planner::compile(PathState start, double override, bool stopping)
    {
        m_segments = 0;
        if (m_stop.stop_requested()) {
            return std::unexpected(RobotError::Cancelled);
        }
        if (!m_prepared || !std::isfinite(override) || override <= 0 || override > 1 ||
            !std::isfinite(start.position) || start.position < 0 || start.position >= 1 ||
            !std::isfinite(start.velocity) || start.velocity < 0 || !std::isfinite(start.acceleration)) {
            return std::unexpected(RobotError::InvalidInput);
        }
        const auto limits{ scalarLimits(override) };
        const auto profile{ stopping ? ::motion::Profile::brake(start.velocity, start.acceleration, limits)
                                     : ::motion::Profile::create(
                                         1 - start.position, limits, start.velocity, start.acceleration, 0) };
        if (!profile || profile->distance() > 1 - start.position + PROGRESS_EPS) {
            return std::unexpected(RobotError::NoStoppingPath);
        }
        m_profile = *profile;
        m_offset = start.position;
        m_finalProgress = start.position + profile->distance();
        if (profile->duration() <= 0 || !std::isfinite(profile->duration()) ||
            profile->duration() > COMPILED_DURATION_LIMIT_S) {
            return std::unexpected(RobotError::Timing);
        }
        m_durationTicks = static_cast<std::uint64_t>(std::ceil(profile->duration() / tr::TICK_SECONDS));
        std::array<std::uint64_t, MAX_BOUNDARIES> boundaries{};
        std::size_t size{ 0 };
        double time{ 0 };
        const auto description{ profile->description() };
        for (unsigned i{ 0 }; i < description.size; ++i) {
            time += description.phases[i].duration;
            boundaries[size++] =
              std::min(m_durationTicks, static_cast<std::uint64_t>(std::ceil(time / tr::TICK_SECONDS)));
        }
        for (std::size_t i{ 0 }; i < m_pieces; ++i) {
            const double end{ m_workspace->pieces[i].end };
            if (end > start.position && end < m_finalProgress) {
                boundaries[size++] = static_cast<std::uint64_t>(
                  std::ceil(profile->timeAt(end - start.position) / tr::TICK_SECONDS));
            }
        }
        std::sort(boundaries.begin(), boundaries.begin() + static_cast<std::ptrdiff_t>(size));
        std::uint64_t previous{ 0 };
        for (std::size_t i{ 0 }; i < size; ++i) {
            if (m_stop.stop_requested()) {
                return std::unexpected(RobotError::Cancelled);
            }
            const auto error{ compileInterval(previous, boundaries[i], 0) };
            if (error != RobotError::None) {
                m_segments = 0;
                return std::unexpected(error);
            }
            previous = boundaries[i];
        }
        if (m_segments == 0) {
            return std::unexpected(RobotError::InvalidInput);
        }
        PlanSummary result{ .segments = m_segments,
                            .ticks = m_durationTicks,
                            .duration = static_cast<double>(m_durationTicks) * tr::TICK_SECONDS,
                            .quantization_bound = m_model->quantizationBound(),
                            .path_error_bound = m_model->configuration().path_tolerance.get(),
                            .final_progress = m_finalProgress,
                            .branch = m_branch };
        const auto finish{ m_workspace->segments[m_segments - 1].at(1) };
        for (unsigned i{ 0 }; i < 3; ++i) {
            result.endpoint[i] = *tr::quantize(finish[i].position);
        }
        for (const auto& s : segments()) {
            const double t{ static_cast<double>(s.ticks) * tr::TICK_SECONDS };
            for (unsigned i{ 0 }; i < 3; ++i) {
                result.peak_velocity[i] =
                  std::max(result.peak_velocity[i], s.motors[i].range(0, 1, 1).magnitude() / t);
                result.peak_acceleration[i] =
                  std::max(result.peak_acceleration[i], s.motors[i].range(0, 1, 2).magnitude() / (t * t));
                result.peak_jerk[i] =
                  std::max(result.peak_jerk[i], s.motors[i].range(0, 1, 3).magnitude() / (t * t * t));
            }
        }
        return result;
    }
    std::span<const tr::Segment> Planner::segments() const noexcept
    {
        return std::span(m_workspace->segments).first(m_segments);
    }
}
