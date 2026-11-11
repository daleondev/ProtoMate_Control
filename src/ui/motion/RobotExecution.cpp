#include "Axis.hpp"
#include "Robot.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace ui::motion
{
    namespace tr = ::motion::trajectory;
    namespace
    {
        constexpr std::uint8_t ALL_AXES{ 7 }, LINEAR_AXIS{ 4 };
        constexpr double NEUTRAL_SPEED{ .25 }, DYNAMIC_ROUNDOFF{ 1e-12 }, MAX_JOG_SECONDS{ 1e6 };
        tr::Error select_dynamics(const Robot::ExecutionRequest& request, double scale, tr::Limits& limits)
        {
            for (const auto value :
                 { request.speed, request.acceleration, request.deceleration, request.jerk }) {
                if (!std::isfinite(value) || value < 0) {
                    return tr::Error::InvalidInput;
                }
            }
            // Explicit values above machine limits are rejected, never silently clamped.
            const auto select{ [&](double value, double& limit) {
                if (value == 0) {
                    return true;
                }
                if (value * scale > limit * (1 + DYNAMIC_ROUNDOFF)) {
                    return false;
                }
                limit = value * scale;
                return true;
            } };
            if (!select(request.speed, limits.velocity) ||
                !select(request.acceleration, limits.acceleration) ||
                !select(request.deceleration, limits.deceleration) || !select(request.jerk, limits.jerk)) {
                return tr::Error::Configuration;
            }
            return tr::Error::None;
        }
    }
    Axis<pnm::units::Angle> Robot::shoulder() { return { *this, 0 }; }
    Axis<pnm::units::Angle> Robot::elbow() { return { *this, 1 }; }
    Axis<pnm::units::Distance> Robot::z() { return { *this, 2 }; }
    std::uint64_t Robot::moveRel(JointPosition q, Dynamics d, pnm::units::Time t)
    {
        ExecutionRequest r{};
        r.relative = true;
        r.move.kind = MoveKind::Joints;
        r.move.joints = q;
        r.dynamics = d;
        r.timeout = t.get();
        return submit(r);
    }
    std::uint64_t Robot::moveAbs(JointPosition q, Dynamics d, pnm::units::Time t)
    {
        ExecutionRequest r{};
        r.move.kind = MoveKind::Joints;
        r.move.joints = q;
        r.dynamics = d;
        r.timeout = t.get();
        return submit(r);
    }
    std::uint64_t Robot::moveRel(CartesianPosition p, Dynamics d, pnm::units::Time t)
    {
        ExecutionRequest r{};
        r.relative = true;
        r.move.kind = MoveKind::Point;
        r.move.target = p;
        r.dynamics = d;
        r.timeout = t.get();
        return submit(r);
    }
    std::uint64_t Robot::moveAbs(CartesianPosition p, Dynamics d, pnm::units::Time t)
    {
        ExecutionRequest r{};
        r.move.kind = MoveKind::Point;
        r.move.target = p;
        r.dynamics = d;
        r.timeout = t.get();
        return submit(r);
    }
    std::uint64_t Robot::home()
    {
        ExecutionRequest r{};
        r.kind = ExecutionRequest::Kind::Home;
        return submit(r);
    }
    void Robot::stop()
    {
        if (!m_client) {
            throw std::logic_error("motion client unavailable");
        }
        // Telemetry can lag admission or be stale. Stop our owned jobs directly,
        // including a request still waiting in the planning queue. A job-specific
        // cancellation cannot accidentally stop a newly admitted axis command.
        m_client->cancelFamily(::motion::Family::Robot);
    }
    void Robot::stopAxis(unsigned joint)
    {
        if (!m_client) {
            throw std::logic_error("motion client unavailable");
        }
        if (joint >= 3) {
            throw std::invalid_argument("invalid axis");
        }
        unsigned motor{ 0 };
        {
            std::scoped_lock lock{ m_mutex };
            motor = m_motorForJoint[joint];
        }
        m_client->stop(1U << motor);
    }
    std::uint64_t Robot::submit(ExecutionRequest request)
    {
        if (!m_client) {
            throw std::logic_error("motion client unavailable");
        }
        std::scoped_lock lock{ m_mutex };
        if (!m_connected || m_stopped) {
            throw std::runtime_error("IO disconnected");
        }
        auto* slot{ std::ranges::find_if(m_arena->pending, [](const auto& p) { return !p.id; }) };
        if (slot == m_arena->pending.end()) {
            throw std::runtime_error("planning queue full");
        }
        const auto id{ m_client->create(request.family, request.axes, request.source, request.continuous) };
        Client::Requested description{};
        description.relative = request.relative;
        if (request.kind == ExecutionRequest::Kind::Home) {
            description.space = Client::RequestSpace::Home;
        }
        else if ((request.kind == ExecutionRequest::Kind::Jog ||
                  request.kind == ExecutionRequest::Kind::CartesianJog)) {
            description.space = Client::RequestSpace::Jog;
            description.values = request.kind == ExecutionRequest::Kind::CartesianJog ? request.direction :
                std::array<double,3>{request.speed, request.duration, request.continuous ? 1. : 0.};
        }
        else if (request.family == ::motion::Family::Axis) {
            description.space =
              request.axes == LINEAR_AXIS ? Client::RequestSpace::Linear : Client::RequestSpace::Rotary;
            description.values[0] = request.distance;
        }
        else if (request.move.kind == MoveKind::Joints) {
            description.space = Client::RequestSpace::Joints;
            description.values = { request.move.joints.shoulder.get(),
                                   request.move.joints.elbow.get(),
                                   request.move.joints.z.get() };
        }
        else {
            description.space = Client::RequestSpace::Point;
            description.values = { request.move.target.x.get(),
                                   request.move.target.y.get(),
                                   request.move.target.z.get() };
        }
        m_client->describe(id, description);
        *slot = { .request = request, .id = id };
        m_changed.signal();
        return id;
    }
    RobotModel Robot::model(const std::stop_token& stop, RobotModel::Scope scope)
    {
        if (!m_client) {
            throw std::logic_error("motion client unavailable");
        }
        const auto snapshot{ m_client->inspect(stop) };
        const auto config{ execution_configuration(snapshot.capabilities) };
        if (!config) {
            throw std::invalid_argument("axis/robot configuration missing; calibrate mechanics first");
        }
        if (config->purpose == RobotConfiguration::Purpose::Physical && !config->calibrated) {
            throw std::invalid_argument("physical configuration has not been verified");
        }
        RobotModel result{ *config, snapshot.capabilities, scope };
        {
            std::scoped_lock lock{ m_mutex };
            m_motorForJoint = config->motor_for_joint;
        }
        return result;
    }
    bool Robot::executePending(const std::stop_token& stop)
    {
        ExecutionRequest request{};
        std::uint64_t id{};
        {
            std::scoped_lock lock{ m_mutex };
            auto* selected{ m_arena->pending.end() };
            for (auto* it{ m_arena->pending.begin() }; it != m_arena->pending.end(); ++it) {
                if (it->id && (selected == m_arena->pending.end() || it->id < selected->id)) {
                    selected = it;
                }
            }
            if (selected == m_arena->pending.end()) {
                return false;
            }
            request = selected->request;
            id = selected->id;
            selected->id = 0;
        }
        const auto token{ m_client->cancellation(id) };
        std::stop_source source{};
        const std::stop_callback cancel_application{ stop, [&] { source.request_stop(); } };
        const std::stop_callback cancel_job{ token, [&] { source.request_stop(); } };
        try {
            execute(request, id, source.get_token());
        } catch (const std::invalid_argument&) {
            m_client->reject(id, tr::Error::Configuration);
        } catch (const std::out_of_range&) {
            m_client->reject(id, tr::Error::PositionLimit);
        } catch (const std::exception& error) {
            // Keep failure diagnostics nonthrowing, including allocation-failure paths.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,modernize-use-std-print)
            std::fprintf(stderr,
                         "Motion preparation %llu failed: %s\n",
                         static_cast<unsigned long long>(id),
                         error.what());
            m_client->reject(id, source.stop_requested() ? tr::Error::Cancelled : tr::Error::Unknown);
        } catch (...) {
            m_client->reject(id, source.stop_requested() ? tr::Error::Cancelled : tr::Error::Unknown);
        }
        m_client->finishPlanning(id);
        return true;
    }
    void Robot::executeHome(const ExecutionRequest& request, const ExecutionContext& context)
    {
        const auto& mechanics{ context.mechanics.get() };
        const auto& snapshot{ context.snapshot.get() };
        const auto& stop{ context.stop.get() };
        const auto id{ context.job };

        tr::FiniteProgram home{};
        home.kind = tr::ProgramKind::Home;
        for (unsigned stage{ 0 }; stage < 3; ++stage) {
            home.home_order[stage] =
              mechanics.configuration().motor_for_joint[mechanics.configuration().homing_order[stage]];
        }
        for (unsigned i{ 0 }; i < 3; ++i) {
            const auto& h{ mechanics.configuration().homing[i] };
            const auto motor{ mechanics.configuration().motor_for_joint[i] };
            if ((request.axes & (1U << i)) &&
                (!h.configured ||
                 h.positive != bool(mechanics.hardware().reference_forward_mask & (1U << motor)))) {
                m_client->reject(id, tr::Error::Configuration);
                return;
            }
            home.home[motor] = { .seek_rpm = h.seek.get<pnm::units::AngularVelocityUnits::rpm>(),
                                 .latch_rpm = h.latch.get<pnm::units::AngularVelocityUnits::rpm>(),
                                 .timeout = h.timeout.get() };
        }
        if (request.family == ::motion::Family::Robot) {
            if (!mechanics.configuration().neutral_position || request.axes != ALL_AXES) {
                m_client->reject(id, tr::Error::Configuration);
                return;
            }
            home.home_to_neutral = true;
            home.limits = mechanics.motorLimits();
            const auto neutral{ mechanics.toMotors(*mechanics.configuration().neutral_position) };
            for (unsigned i{ 0 }; i < 3; ++i) {
                const auto target{ tr::quantize(neutral[i]) };
                if (!target) {
                    m_client->reject(id, target.error());
                    return;
                }
                home.home_neutral[i] = *target;
                home.limits[i].velocity *= NEUTRAL_SPEED;
            }
        }
        m_client->execute(id, snapshot, mechanics.fingerprint(), home, stop);
    }
    void Robot::executeCartesianJog(const ExecutionRequest& request, const ExecutionContext& context)
    {
        const auto& mechanics{ context.mechanics.get() };
        const auto& snapshot{ context.snapshot.get() };
        const auto& stop{ context.stop.get() };
        const auto id{ context.job };

        if (request.family != ::motion::Family::Robot || request.axes != ALL_AXES ||
            !std::isfinite(request.duration) || !std::isfinite(request.timeout) || request.timeout < 0 ||
            (!request.continuous && request.duration <= 0)) {
            m_client->reject(id, tr::Error::InvalidInput);
            return;
        }
        Planner planner{ mechanics, m_arena->workspace, stop };
        auto program{ planner.cartesianJog(snapshot.position,
                                           request.direction,
                                           request.dynamics.speed,
                                           request.dynamics.acceleration,
                                           request.dynamics.deceleration,
                                           request.dynamics.jerk) };
        if (!program) {
            m_client->reject(id, program.error());
            return;
        }
        program->timeout = request.continuous ? request.timeout : request.duration;
        if (request.timeout > 0 && program->timeout > request.timeout) {
            program->timeout = request.timeout;
        }
        const auto profile{ ::motion::Profile::fromDescription(program->progress) };
        m_client->setPlannedDuration(id, profile ? profile->duration() : 0);
        if (stop.stop_requested()) {
            m_client->reject(id, tr::Error::Cancelled);
            return;
        }
        m_client->execute(id, snapshot, mechanics.fingerprint(), *program, stop);
    }
    std::expected<tr::FiniteProgram, tr::Error> Robot::compileAxis(ExecutionRequest request,
                                                                   const ExecutionContext& context,
                                                                   JointPosition start)
    {
        const auto& mechanics{ context.mechanics.get() };
        const auto& snapshot{ context.snapshot.get() };
        const auto id{ context.job };
        const bool jogging{ request.kind == ExecutionRequest::Kind::Jog };
        tr::FiniteProgram program{};

        const auto axis{ std::countr_zero(unsigned(request.axes)) };
        if (axis >= 3 || std::popcount(unsigned(request.axes)) != 1) {
            return std::unexpected(tr::Error::InvalidInput);
        }
        auto q{ RobotModel::joints(start) };
        if (jogging) {
            const auto limits{ mechanics.jointLimits()[axis] };
            q[axis] = request.speed > 0 ? limits.maximum : limits.minimum;
            request.speed = std::abs(request.speed);
        }
        else {
            q[axis] = request.relative ? q[axis] + request.distance : request.distance;
        }
        const auto end{ mechanics.toMotors(RobotModel::joints(q)) };
        const auto motor{ mechanics.configuration().motor_for_joint[axis] };
        const auto rounded{ tr::quantize(end[motor]) };
        if (!rounded) {
            return std::unexpected(rounded.error());
        }
        program.limits = mechanics.motorLimits();
        const auto endpoint{ jogging ? std::clamp(*rounded,
                                                  std::int64_t(std::ceil(program.limits[motor].minimum)),
                                                  std::int64_t(std::floor(program.limits[motor].maximum)))
                                     : *rounded };
        program.displacement[motor] = endpoint - snapshot.position[motor];
        auto& limits{ program.limits[motor] };
        const auto scale{ std::abs(mechanics.stepsPerJointUnit()[axis]) };
        if (const auto error{ select_dynamics(request, scale, limits) }; error != tr::Error::None) {
            return std::unexpected(error);
        }
        const auto d{ std::abs(double(program.displacement[motor])) };
        if (d != 0) {
            program.progress_limits = { .velocity = limits.velocity / d,
                                        .acceleration = limits.acceleration / d,
                                        .deceleration = limits.deceleration / d,
                                        .jerk = limits.jerk / d };
            const auto profile{ ::motion::Profile::create(1., program.progress_limits) };
            if (profile) {
                m_client->setPlannedDuration(id, profile->duration());
            }
            if (!profile || profile->duration() >
                              (jogging ? MAX_JOG_SECONDS : ::motion::operating::MAX_PROGRAM_SECONDS)) {
                return std::unexpected(tr::Error::Timing);
            }
            program.progress = profile->description();
        }
        return program;
    }
    std::expected<tr::FiniteProgram, tr::Error> Robot::compileRobot(const ExecutionRequest& request,
                                                                    const ExecutionContext& context,
                                                                    JointPosition start)
    {
        const auto& mechanics{ context.mechanics.get() };
        const auto& snapshot{ context.snapshot.get() };
        const auto& stop{ context.stop.get() };
        const auto id{ context.job };

        auto move{ request.move };
        move.speed = request.dynamics.speed;
        if (request.relative) {
            if (move.kind == MoveKind::Joints) {
                move.joints.shoulder += start.shoulder;
                move.joints.elbow += start.elbow;
                move.joints.z += start.z;
            }
            else if (move.kind == MoveKind::Point) {
                const auto tcp{ mechanics.kinematics().forward(start) };
                if (!tcp) {
                    return std::unexpected(tr::Error::Unreachable);
                }
                const auto delta{ move.frame == CoordinateFrame::Work ? mechanics.vectorFromWork(move.target)
                                                                      : move.target };
                move.target = { .x = tcp->position.x + delta.x,
                                .y = tcp->position.y + delta.y,
                                .z = tcp->position.z + delta.z };
                move.frame = CoordinateFrame::Base;
            }
        }
        Planner planner{ mechanics, m_arena->workspace, stop };
        const auto error{ planner.prepare(move, snapshot.position) };
        if (error != tr::Error::None) {
            return std::unexpected(error);
        }
        double estimated_seconds{};
        const auto compiled{ planner.finite(request.dynamics.acceleration,
                                            request.dynamics.deceleration,
                                            request.dynamics.jerk,
                                            &estimated_seconds) };
        m_client->setPlannedDuration(id, estimated_seconds);
        if (!compiled) {
            return std::unexpected(compiled.error());
        }
        return *compiled;
    }
    void Robot::execute(ExecutionRequest request, std::uint64_t id, const std::stop_token& stop)
    {
        if (stop.stop_requested()) {
            m_client->reject(id, tr::Error::Cancelled);
            return;
        }
        const auto mechanics{ model(stop,
                                    request.family == ::motion::Family::Axis ? RobotModel::Scope::Axes
                                                                             : RobotModel::Scope::Robot) };
        std::uint8_t motors{};
        for (unsigned joint{ 0 }; joint < 3; ++joint) {
            if (request.axes & (1U << joint)) {
                motors |= 1U << mechanics.configuration().motor_for_joint[joint];
            }
        }
        const auto snapshot{ m_client->acquire(
          { .id = id, .configuration = mechanics.fingerprint(), .axes = motors }, stop) };
        if (snapshot.error != tr::Error::None) {
            return;
        }
        const ExecutionContext context{
            .mechanics = mechanics, .snapshot = snapshot, .job = id, .stop = stop
        };
        if (request.kind == ExecutionRequest::Kind::Home) {
            executeHome(request, context);
            return;
        }
        if (request.kind == ExecutionRequest::Kind::CartesianJog) {
            executeCartesianJog(request, context);
            return;
        }
        const bool jogging{ request.kind == ExecutionRequest::Kind::Jog };
        if (jogging && (request.family != ::motion::Family::Axis || !std::isfinite(request.speed) ||
                        request.speed == 0 || !std::isfinite(request.duration) ||
                        (!request.continuous && request.duration <= 0))) {
            m_client->reject(id, tr::Error::InvalidInput);
            return;
        }
        tr::Positions counts{};
        for (unsigned i{ 0 }; i < 3; ++i) {
            counts[i] = static_cast<double>(snapshot.position[i]);
        }
        auto start{ mechanics.toJoints(counts) };
        auto compiled{ request.family == ::motion::Family::Axis ? compileAxis(request, context, start)
                                                                : compileRobot(request, context, start) };
        if (!compiled) {
            m_client->reject(id, compiled.error());
            return;
        }
        auto& program{ *compiled };

        program.kind = jogging ? tr::ProgramKind::Jog : tr::ProgramKind::Finite;
        program.timeout = request.timeout;
        if (jogging) {
            program.timeout = request.continuous ? 0 : request.duration;
        }
        if (stop.stop_requested()) {
            m_client->reject(id, tr::Error::Cancelled);
            return;
        }
        m_client->execute(id, snapshot, mechanics.fingerprint(), program, stop);
    }
}
