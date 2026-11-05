#include "Planner.hpp"

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
        constexpr double PI{ std::numbers::pi };
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
        RobotError kinematicError(ScaraKinematics::Error error)
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
    }

    Planner::Planner(const RobotModel& model, Workspace& workspace, std::stop_token stop)
      : m_model{ &model }
      , m_stop{ std::move(stop) }
      , m_workspace{ &workspace }
    {
    }

    RobotError Planner::prepare(const Move& move, const tr::Counts& counts)
    {
        m_prepared = false;
        if (m_stop.stop_requested()) {
            return RobotError::Cancelled;
        }
        m_pieces = 0;
        m_segments = 0;
        m_move = move;
        if (!std::isfinite(move.speed) || move.speed <= 0 || move.speed > 1 || !move.feed.isFinite() ||
            move.feed.get() < 0 || !move.blend.isFinite() || move.blend.get() < 0 ||
            move.kind > MoveKind::CartesianCurve || move.frame > CoordinateFrame::Work) {
            return RobotError::InvalidInput;
        }
        if (m_model->quantizationBound() >= m_model->configuration().path_tolerance.get()) {
            return RobotError::PathTolerance;
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
            return kinematicError(branch.error());
        }
        m_branch = *branch;
        const auto from{ m_model->kinematics().forward(m_seed) };
        if (!from) {
            return kinematicError(from.error());
        }
        const auto q0{ m_model->joints(m_seed) };
        JointPosition target{ move.joints };
        m_cartesianPath = move.kind == MoveKind::Linear || move.kind == MoveKind::CartesianCurve;
        if (move.kind == MoveKind::Point || move.kind == MoveKind::Linear) {
            const auto endpoint{ move.frame == CoordinateFrame::Work ? m_model->fromWork(move.target)
                                                                     : move.target };
            const auto solution{ m_model->kinematics().inverse(endpoint, m_seed, m_branch) };
            if (!solution) {
                return kinematicError(solution.error());
            }
            target = *solution;
            const auto a{ xyz(from->position) }, b{ xyz(endpoint) };
            for (unsigned i{ 0 }; i < 3; ++i) {
                m_cartesian[i] = { { a[i], b[i] - a[i] } };
            }
        }
        else if (move.kind == MoveKind::CartesianCurve) {
            m_cartesian = move.curve;
        }
        if (m_cartesianPath) {
            const auto first{ inverseAt(0) };
            if (!first) {
                return first.error();
            }
            for (unsigned i{ 0 }; i < 3; ++i) {
                if (std::abs((*first)[i].position - q0[i]) > 1e-8) {
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
        }
        else {
            auto& piece{ m_workspace->pieces[0] };
            piece.begin = 0;
            piece.end = 1;
            if (move.kind == MoveKind::JointCurve) {
                piece.joints = move.curve;
                for (unsigned i{ 0 }; i < 3; ++i) {
                    if (std::abs(piece.joints[i].at(0) - q0[i]) > 1e-8) {
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
                const auto q1{ m_model->joints(m_model->toJoints(motor)) };
                for (unsigned i{ 0 }; i < 3; ++i) {
                    piece.joints[i] = { { q0[i], q1[i] - q0[i] } };
                }
            }
            unsigned budget{ 8192 };
            const auto error{ certify(piece.joints, nullptr, 0, 1, 0, budget) };
            if (error != RobotError::None) {
                return error;
            }
            m_pieces = 1;
            m_pathDistance = 0;
        }
        m_prepared = true;
        return RobotError::None;
    }

    std::expected<Planner::JointState, RobotError> Planner::inverseAt(double s) const
    {
        std::array<double, 3> p{}, v{}, a{};
        for (unsigned i{ 0 }; i < 3; ++i) {
            p[i] = m_cartesian[i].at(s);
            v[i] = m_cartesian[i].at(s, 1);
            a[i] = m_cartesian[i].at(s, 2);
        }
        const auto q{ m_model->kinematics().inverse(point(p), m_seed, m_branch) };
        if (!q) {
            return std::unexpected(kinematicError(q.error()));
        }
        const auto velocity{ m_model->kinematics().inverseVelocity(
          *q, { Velocity::create(v[0]), Velocity::create(v[1]), Velocity::create(v[2]) }) };
        if (!velocity) {
            return std::unexpected(kinematicError(velocity.error()));
        }
        const auto pos{ m_model->joints(*q) }, speed{ components(*velocity) };
        const auto& g{ m_model->configuration().geometry };
        const double first{ g.base_yaw.get() + pos[0] };
        const double phase{ std::atan2(g.tool_offset.y.get(),
                                       (g.second_arm_length + g.tool_offset.x).get()) };
        const double second{ first + pos[1] + phase };
        const double l1{ g.first_arm_length.get() },
          l2{ std::hypot((g.second_arm_length + g.tool_offset.x).get(), g.tool_offset.y.get()) };
        const double w1{ speed[0] * speed[0] }, w2{ (speed[0] + speed[1]) * (speed[0] + speed[1]) };
        // J*q'' = p'' - Jdot*q'. inverseVelocity applies the same inverse J.
        const auto acceleration{ m_model->kinematics().inverseVelocity(
          *q,
          { Velocity::create(a[0] + l1 * std::cos(first) * w1 + l2 * std::cos(second) * w2),
            Velocity::create(a[1] + l1 * std::sin(first) * w1 + l2 * std::sin(second) * w2),
            Velocity::create(a[2]) }) };
        if (!acceleration) {
            return std::unexpected(kinematicError(acceleration.error()));
        }
        const auto acc{ components(*acceleration) };
        JointState result{};
        for (unsigned i{ 0 }; i < 3; ++i) {
            result[i] = { pos[i], speed[i], acc[i] };
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
        std::array<Interval, 18> work{};
        work[0] = { from, to, depth };
        std::size_t pending{ 1 };
        unsigned visits{ 0 };
        while (pending) {
            if (m_stop.stop_requested()) {
                return RobotError::Cancelled;
            }
            if (++visits > 8192) {
                return RobotError::SubdivisionLimit;
            }
            const auto interval{ work[--pending] };
            from = interval.from;
            to = interval.to;
            depth = interval.depth;
            const auto a{ inverseAt(from) }, b{ inverseAt(to) };
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
            unsigned budget{ 8192 };
            const auto error{ certify(piece.joints, &desired, 0, 1, 0, budget) };
            if (error == RobotError::None) {
                if (m_pieces == m_workspace->pieces.size()) {
                    return RobotError::Capacity;
                }
                m_workspace->pieces[m_pieces++] = piece;
                continue;
            }
            if (depth >= 16) {
                return error;
            }
            const double middle{ (from + to) * 0.5 };
            const auto mid{ inverseAt(middle) };
            if (!mid) {
                return mid.error();
            }
            work[pending++] = { middle, to, depth + 1 };
            work[pending++] = { from, middle, depth + 1 };
        }
        return RobotError::None;
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
        std::array<Interval, 18> work{};
        work[0] = { from, to, depth };
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
            const double middle{ (from + to) * 0.5 };
            std::array<double, 3> position{};
            std::array<I, 3> ranges{}, derivatives{};
            const auto limits{ m_model->jointLimits() };
            bool proven{ true };
            RobotError reason{ RobotError::PositionLimit };
            for (unsigned i{ 0 }; i < 3; ++i) {
                if (!q[i].finite()) {
                    return RobotError::InvalidInput;
                }
                position[i] = q[i].at(middle);
                ranges[i] = q[i].range(from, to);
                derivatives[i] = q[i].range(from, to, 1);
                if (position[i] < limits[i].minimum - GEOMETRY_EPS ||
                    position[i] > limits[i].maximum + GEOMETRY_EPS) {
                    return RobotError::PositionLimit;
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
                return RobotError::Singularity;
            }
            const bool bendValid{ sign > 0 ? bend.minimum > g.minimum_bend_sine
                                           : bend.maximum < -g.minimum_bend_sine };
            if (!bendValid) {
                proven = false;
                reason = RobotError::Singularity;
            }
            const auto a{ shift(ranges[0], g.base_yaw.get()) },
              b{ shift(add(ranges[0], ranges[1]), g.base_yaw.get() + phase) };
            const double l1{ g.first_arm_length.get() },
              l2{ std::hypot((g.second_arm_length + g.tool_offset.x).get(), g.tool_offset.y.get()) };
            std::array<I, 3> tcp{ shift(add(scale(cosine(a), l1), scale(cosine(b), l2)),
                                        g.base_origin.x.get()),
                                  shift(add(scale(sine(a), l1), scale(sine(b), l2)), g.base_origin.y.get()),
                                  shift(ranges[2], (g.base_origin.z + g.tool_offset.z).get()) };
            const auto fk{ m_model->kinematics().forward(m_model->joints(position)) };
            if (!fk) {
                return kinematicError(fk.error());
            }
            const auto actual{ xyz(fk->position) }, lo{ xyz(m_model->configuration().workspace_minimum) },
              hi{ xyz(m_model->configuration().workspace_maximum) };
            for (unsigned i{ 0 }; i < 3; ++i) {
                if (actual[i] < lo[i] - GEOMETRY_EPS || actual[i] > hi[i] + GEOMETRY_EPS) {
                    return RobotError::Workspace;
                }
                if (tcp[i].minimum < lo[i] - GEOMETRY_EPS || tcp[i].maximum > hi[i] + GEOMETRY_EPS) {
                    proven = false;
                    reason = RobotError::Workspace;
                }
            }
            if (desired) {
                const auto jointSum{ add(derivatives[0], derivatives[1]) };
                const std::array<I, 3> velocity{ add(scale(multiply(sine(a), derivatives[0]), -l1),
                                                     scale(multiply(sine(b), jointSum), -l2)),
                                                 add(scale(multiply(cosine(a), derivatives[0]), l1),
                                                     scale(multiply(cosine(b), jointSum), l2)),
                                                 derivatives[2] };
                double deviation2{ 0 };
                for (unsigned i{ 0 }; i < 3; ++i) {
                    const auto expected{ (*desired)[i].range(from, to, 1) };
                    const auto difference{ add(velocity[i], scale(expected, -1)) };
                    const double bound{ std::abs(actual[i] - (*desired)[i].at(middle)) +
                                        difference.magnitude() * (to - from) * 0.5 + GEOMETRY_EPS };
                    deviation2 += bound * bound;
                }
                const double available{
                    (m_model->configuration().path_tolerance.get() - m_model->quantizationBound()) * 0.45
                };
                if (std::sqrt(deviation2) > available) {
                    proven = false;
                    reason = RobotError::PathTolerance;
                }
            }
            if (proven) {
                continue;
            }
            if (depth >= 16 || to - from < 1e-9) {
                return reason;
            }
            work[pending++] = { middle, to, depth + 1 };
            work[pending++] = { from, middle, depth + 1 };
        }
        return RobotError::None;
    }

    ::motion::Profile::Limits Planner::scalarLimits(double speed) const
    {
        const auto limits{ m_model->jointLimits() };
        std::array<double, 3> d1{}, d2{}, d3{};
        for (std::size_t n{ 0 }; n < m_pieces; ++n) {
            const auto& p{ m_workspace->pieces[n] };
            const double w{ p.end - p.begin };
            for (unsigned i{ 0 }; i < 3; ++i) {
                d1[i] = std::max(d1[i], p.joints[i].range(0, 1, 1).magnitude() / w);
                d2[i] = std::max(d2[i], p.joints[i].range(0, 1, 2).magnitude() / (w * w));
                d3[i] = std::max(d3[i], p.joints[i].range(0, 1, 3).magnitude() / (w * w * w));
            }
        }
        ::motion::Profile::Limits scalar{ 1e6, 1e6, 1e6, 1e6 };
        for (unsigned i{ 0 }; i < 3; ++i) {
            if (d1[i] < 1e-12) {
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
        const double v{ scalar.velocity }, a{ std::max(scalar.acceleration, scalar.deceleration) },
          j{ scalar.jerk };
        for (unsigned i{ 0 }; i < 3; ++i) {
            stretch = std::max(stretch, d1[i] * v / limits[i].velocity);
            stretch = std::max(stretch,
                               std::sqrt((d2[i] * v * v + d1[i] * a) /
                                         std::min(limits[i].acceleration, limits[i].deceleration)));
            stretch = std::max(
              stretch, std::cbrt((d3[i] * v * v * v + 3 * d2[i] * v * a + d1[i] * j) / limits[i].jerk));
        }
        // Reserve margin for tick quantization and the time-domain interpolation.
        stretch *= 1.25;
        scalar.velocity /= stretch;
        scalar.acceleration /= stretch * stretch;
        scalar.deceleration /= stretch * stretch;
        scalar.jerk /= stretch * stretch * stretch;
        return scalar;
    }

    Planner::JointState Planner::jointAt(double s, double velocity, double acceleration) const
    {
        std::size_t index{ 0 };
        while (index + 1 < m_pieces && s > m_workspace->pieces[index].end) {
            ++index;
        }
        const auto& p{ m_workspace->pieces[index] };
        const double w{ p.end - p.begin };
        const double u{ std::clamp((s - p.begin) / w, 0.0, 1.0) };
        JointState out{};
        for (unsigned i{ 0 }; i < 3; ++i) {
            const double d1{ p.joints[i].at(u, 1) / w }, d2{ p.joints[i].at(u, 2) / (w * w) };
            out[i] = { p.joints[i].at(u), d1 * velocity, d2 * velocity * velocity + d1 * acceleration };
        }
        return out;
    }
    PathState Planner::pathAt(double seconds) const noexcept
    {
        const auto s{ m_profile.at(seconds) };
        return { m_offset + s.position, s.velocity, s.acceleration };
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
        const auto zero{ m_model->toMotors(m_model->joints(std::array<double, 3>{})) };
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
        unsigned budget{ 8192 };
        return certify(joints, reference, 0, 1, 0, budget);
    }

    RobotError Planner::compileInterval(std::uint64_t from, std::uint64_t to, unsigned depth)
    {
        struct Interval
        {
            std::uint64_t from, to;
            unsigned depth;
        };
        std::array<Interval, 22> work{};
        work[0] = { from, to, depth };
        std::size_t pending{ 1 };
        unsigned visits{ 0 };
        while (pending) {
            if (m_stop.stop_requested()) {
                return RobotError::Cancelled;
            }
            if (++visits > 8192) {
                return RobotError::SubdivisionLimit;
            }
            const auto interval{ work[--pending] };
            from = interval.from;
            to = interval.to;
            depth = interval.depth;
            if (to <= from) {
                continue;
            }
            const auto a{ pathAt(from * tr::TICK_SECONDS) }, b{ pathAt(to * tr::TICK_SECONDS) };
            auto start{ m_model->toMotors(jointAt(a.position, a.velocity, a.acceleration)) };
            auto end{ m_model->toMotors(jointAt(b.position, b.velocity, b.acceleration)) };
            if (to == m_durationTicks) {
                for (auto& s : end) {
                    const auto rounded{ tr::quantize(s.position) };
                    if (!rounded) {
                        return rounded.error();
                    }
                    s.position = static_cast<double>(*rounded);
                }
            }
            tr::Segment segment{
                .ticks = to - from, .job = m_move.job, .path_begin = a.position, .path_end = b.position
            };
            const double duration{ segment.ticks * tr::TICK_SECONDS };
            for (unsigned i{ 0 }; i < 3; ++i) {
                segment.motors[i] = ::motion::hermite(start[i], end[i], duration);
            }
            std::array<P, 3> reference{};
            if (m_cartesianPath) {
                // Fit the exact requested Cartesian curve in time using the same
                // endpoint derivatives. For MoveLinear this is the scalar time law.
                for (unsigned i{ 0 }; i < 3; ++i) {
                    const auto& p{ m_cartesian[i] };
                    const ::motion::KinematicState p0{ p.at(a.position),
                                                       p.at(a.position, 1) * a.velocity,
                                                       p.at(a.position, 2) * a.velocity * a.velocity +
                                                         p.at(a.position, 1) * a.acceleration };
                    const ::motion::KinematicState p1{ p.at(b.position),
                                                       p.at(b.position, 1) * b.velocity,
                                                       p.at(b.position, 2) * b.velocity * b.velocity +
                                                         p.at(b.position, 1) * b.acceleration };
                    reference[i] = ::motion::hermite(p0, p1, duration);
                }
            }
            auto error{ validatePath(segment, m_cartesianPath ? &reference : nullptr) };
            if (error == RobotError::None && m_cartesianPath) {
                unsigned budget{ 8192 };
                error = certifyReference(reference, from * tr::TICK_SECONDS, duration, 0, 1, 0, budget);
            }
            if (error == RobotError::None) {
                if (m_segments == m_workspace->segments.size()) {
                    return RobotError::Capacity;
                }
                if (m_segments &&
                    !tr::continuous(m_workspace->segments[m_segments - 1].at(1), segment.at(0))) {
                    return RobotError::Discontinuity;
                }
                m_workspace->segments[m_segments++] = segment;
                continue;
            }
            if (depth >= 20 || to - from <= 10) {
                return error;
            }
            const auto middle{ from + (to - from) / 2 };
            work[pending++] = { middle, to, depth + 1 };
            work[pending++] = { from, middle, depth + 1 };
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
        std::array<Interval, 18> work{};
        work[0] = { from, to, depth };
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
            const double t0{ start + duration * from }, t1{ start + duration * to },
              middle{ (from + to) * 0.5 };
            const auto a{ pathAt(t0) }, b{ pathAt(t1) }, mid{ pathAt(start + duration * middle) };
            // Bound the actual scalar velocity on every intersected constant-jerk
            // phase. This also covers intervals straddling tick-rounded phase ends.
            I speed{ std::min(a.velocity, b.velocity), std::max(a.velocity, b.velocity) };
            double phaseStart{ 0 };
            const auto description{ m_profile.description() };
            for (unsigned i{ 0 }; i < description.size; ++i) {
                const auto& phase{ description.phases[i] };
                const double lo{ std::max(t0 - phaseStart, 0.0) },
                  hi{ std::min(t1 - phaseStart, phase.duration) };
                if (lo <= hi) {
                    const auto v{ P{ { phase.velocity, phase.acceleration, phase.jerk * 0.5 } }.range(lo,
                                                                                                      hi) };
                    speed.minimum = std::min(speed.minimum, v.minimum);
                    speed.maximum = std::max(speed.maximum, v.maximum);
                }
                phaseStart += phase.duration;
            }
            double squared{ 0 };
            for (unsigned i{ 0 }; i < 3; ++i) {
                const auto exact{ multiply(m_cartesian[i].range(a.position, b.position, 1),
                                           scale(speed, duration)) };
                const auto derivative{ add(exact, scale(reference[i].range(from, to, 1), -1)) };
                const double bound{ std::abs(m_cartesian[i].at(mid.position) - reference[i].at(middle)) +
                                    derivative.magnitude() * (to - from) * 0.5 + GEOMETRY_EPS };
                squared += bound * bound;
            }
            const double available{
                (m_model->configuration().path_tolerance.get() - m_model->quantizationBound()) * 0.45
            };
            if (std::sqrt(squared) <= available) {
                continue;
            }
            if (depth >= 16) {
                return RobotError::SubdivisionLimit;
            }
            work[pending++] = { middle, to, depth + 1 };
            work[pending++] = { from, middle, depth + 1 };
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
        if (!profile || profile->distance() > 1 - start.position + 1e-12) {
            return std::unexpected(RobotError::NoStoppingPath);
        }
        m_profile = *profile;
        m_offset = start.position;
        m_finalProgress = start.position + profile->distance();
        if (profile->duration() <= 0 || !std::isfinite(profile->duration()) || profile->duration() > 3600) {
            return std::unexpected(RobotError::Timing);
        }
        m_durationTicks = static_cast<std::uint64_t>(std::ceil(profile->duration() / tr::TICK_SECONDS));
        std::array<std::uint64_t, 160> boundaries{};
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
                            .duration = m_durationTicks * tr::TICK_SECONDS,
                            .quantization_bound = m_model->quantizationBound(),
                            .path_error_bound = m_model->configuration().path_tolerance.get(),
                            .final_progress = m_finalProgress,
                            .branch = m_branch };
        const auto finish{ m_workspace->segments[m_segments - 1].at(1) };
        for (unsigned i{ 0 }; i < 3; ++i) {
            result.endpoint[i] = *tr::quantize(finish[i].position);
        }
        for (const auto& s : segments()) {
            const double t{ s.ticks * tr::TICK_SECONDS };
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
