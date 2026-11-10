#include "motion.hpp"
#include "Parser.hpp"
#include "ui/motion/Client.hpp"
#include "ui/motion/Robot.hpp"
#include <charconv>
#include <cmath>
#include <format>
#include <functional>
#include <limits>
#include <numbers>
#include <ostream>
#include <stdexcept>

namespace ui::cli::motion
{
    using namespace pnm::meta::string::literals;
    using namespace pnm::units::literals;
    namespace
    {
        constexpr double DEGREES_PER_HALF_TURN{ 180 }, METRES_PER_MILLIMETRE{ .001 },
          MILLIMETRES_PER_METRE{ 1000 };
        constexpr double MILLISECONDS_PER_SECOND{ 1000 }, MAX_WAIT_SECONDS{ 86400 }, DEFAULT_JOG_FACTOR{ .1 };
        using Client = ui::motion::Client;
        using Robot = ui::motion::Robot;
        using Request = Robot::ExecutionRequest;
        struct Context
        {
            std::reference_wrapper<Client> client;
            std::reference_wrapper<Robot> robot;
            std::stop_token stop;
        };
        double number(std::string_view text)
        {
            double n{};
            const auto r{ std::from_chars(text.data(), text.data() + text.size(), n) };
            if (r.ec != std::errc{} || r.ptr != text.data() + text.size() || !std::isfinite(n)) {
                throw std::invalid_argument("expected a finite number");
            }
            return n;
        }
        std::uint64_t identifier(std::string_view text)
        {
            std::uint64_t n{};
            const auto r{ std::from_chars(text.data(), text.data() + text.size(), n) };
            if (r.ec != std::errc{} || r.ptr != text.data() + text.size() || !n) {
                throw std::invalid_argument("invalid job ID");
            }
            return n;
        }
        unsigned axis(std::string_view name)
        {
            if (name == "shoulder") {
                return 0;
            }
            if (name == "elbow") {
                return 1;
            }
            if (name == "z") {
                return 2;
            }
            throw std::invalid_argument("axis must be shoulder, elbow or z");
        }
        double option(const Arguments& args, std::string_view name, double fallback = 0)
        {
            if (args.flagCount(name) > 1) {
                throw std::invalid_argument("repeated option");
            }
            const auto value{ args.flagValue(name) };
            const auto n{ value ? number(*value) : fallback };
            if (n < 0) {
                throw std::invalid_argument("dynamics and timeout must be nonnegative");
            }
            return n;
        }
        const char* family(::motion::Family f)
        {
            switch (f) {
                case ::motion::Family::None:
                    return "none";
                case ::motion::Family::Motor:
                    return "motor";
                case ::motion::Family::Axis:
                    return "axis";
                case ::motion::Family::Robot:
                    return "robot";
            }
            return "unknown";
        }
        const char* state(Client::JobState s)
        {
            using enum Client::JobState;
            switch (s) {
                case Submitting:
                    return "submitting";
                case Planning:
                    return "planning";
                case Preparing:
                    return "preparing";
                case Accepted:
                    return "accepted";
                case Running:
                    return "running";
                case Stopping:
                    return "stopping";
                case Completed:
                    return "completed";
                case Stopped:
                    return "stopped";
                case TimedOut:
                    return "timed out";
                case Rejected:
                    return "rejected";
                case Faulted:
                    return "faulted";
                case Unknown:
                    return "unknown; stop unconfirmed";
            }
            return "unknown";
        }
        void print_job(const Client::JobInfo& j, std::ostream& out)
        {
            out << j.id << ": " << family(j.family) << " " << state(j.state) << "; axes=" << unsigned(j.axes);
            if (j.error != ::motion::trajectory::Error::None) {
                out << "; " << ::motion::trajectory::name(j.error);
            }
            if (j.planned_seconds > 0) {
                out << std::format("; planned {:.3f} s", j.planned_seconds);
            }
            if (j.error == ::motion::trajectory::Error::Timing &&
                j.planned_seconds > ::motion::operating::MAX_PROGRAM_SECONDS) {
                out << " (finite-program maximum is 90 s; shorten the move or increase speed within limits)";
            }
            if (j.busy_job && j.error == ::motion::trajectory::Error::Busy) {
                out << "; owned by job " << j.busy_job << " (motion wait " << j.busy_job
                    << " or motion cancel " << j.busy_job << ")";
            }
            if (j.family != ::motion::Family::Motor &&
                (j.state == Client::JobState::Running || j.state == Client::JobState::Completed)) {
                out << "; quantized endpoint (motor steps)=" << j.endpoint[0] << ',' << j.endpoint[1] << ','
                    << j.endpoint[2];
            }
            out << '\n';
        }
        CallbackResult submit(Context& c, Request request, std::ostream& out)
        {
            const auto id{ c.robot.get().submit(request) };
            out << "Submitted motion job " << id << "; IO acceptance is pending. Use 'motion jobs " << id
                << "'.\n";
            return 0;
        }
        CallbackResult axis_move(Context& c, const Arguments& a, std::ostream& out, bool relative)
        {
            const auto index{ axis(a.require("axis")) };
            const double unit{ index == 2 ? METRES_PER_MILLIMETRE
                                          : std::numbers::pi / DEGREES_PER_HALF_TURN };
            Request r{};
            r.family = ::motion::Family::Axis;
            r.axes = std::uint8_t(1U << index);
            r.relative = relative;
            r.distance = number(a.require("value")) * unit;
            r.speed = option(a, "speed") * unit;
            r.acceleration = option(a, "accel") * unit;
            r.deceleration = option(a, "decel") * unit;
            r.jerk = option(a, "jerk") * unit;
            r.timeout = option(a, "timeout");
            return submit(c, r, out);
        }
        CallbackResult robot_move(Context& c,
                                  const Arguments& a,
                                  std::ostream& out,
                                  bool relative,
                                  bool point)
        {
            Request r{};
            r.relative = relative;
            r.move.kind = point ? ui::motion::MoveKind::Point : ui::motion::MoveKind::Joints;
            const auto x{ number(a.require("a")) };
            const auto y{ number(a.require("b")) };
            const auto z{ number(a.require("c")) };
            if (point) {
                r.move.target = { .x = x * 1_mm, .y = y * 1_mm, .z = z * 1_mm };
            }
            else {
                r.move.joints = { .shoulder = x * 1_deg, .elbow = y * 1_deg, .z = z * 1_mm };
            }
            const auto factor{ [&](std::string_view name, double fallback) {
                const auto f{ option(a, name, fallback) };
                if (f > 1) {
                    throw std::invalid_argument(
                      "robot dynamics factors must be in (0,1]; zero selects default");
                }
                return f == 0 ? fallback : f;
            } };
            r.dynamics = { .speed = factor("speed", Robot::Dynamics::DEFAULT_SPEED),
                           .acceleration = factor("accel", 1),
                           .deceleration = factor("decel", 1),
                           .jerk = factor("jerk", 1) };
            r.timeout = option(a, "timeout");
            return submit(c, r, out);
        }
        CallbackResult positions(Context& c, const Arguments& a, std::ostream& out, bool robot)
        {
            const auto state{ c.client.get().state() };
            const auto model{ c.robot.get().model(
              c.stop, robot ? ui::motion::RobotModel::Scope::Robot : ui::motion::RobotModel::Scope::Axes) };
            ::motion::trajectory::Positions counts{};
            for (unsigned i{ 0 }; i < 3; ++i) {
                counts[i] = state.motors[i].position * std::numbers::pi / DEGREES_PER_HALF_TURN /
                            model.hardware().step_radians[i];
            }
            const auto q{ model.toJoints(counts) };
            const auto values{ ::ui::motion::RobotModel::joints(q) };
            const auto limits{ model.jointLimits() };
            const auto chosen{ a.get("axis") };
            const auto selected{ !chosen || *chosen == "all" ? 3 : axis(*chosen) };
            constexpr std::array names{ "shoulder", "elbow", "z" };
            for (unsigned i{ 0 }; i < 3; ++i) {
                if (selected == 3 || selected == i) {
                    const auto scale{ i == 2 ? MILLIMETRES_PER_METRE
                                             : DEGREES_PER_HALF_TURN / std::numbers::pi };
                    const auto motor{ model.configuration().motor_for_joint[i] };
                    out << std::format("{}: {:.5f} {}; limits [{:.5f}, {:.5f}]; referenced={}; owner={}\n",
                                       names[i],
                                       values[i] * scale,
                                       i == 2 ? "mm" : "deg",
                                       limits[i].minimum * scale,
                                       limits[i].maximum * scale,
                                       state.motors[motor].referenced,
                                       state.owners[motor]);
                }
            }
            if (robot) {
                const auto fk{ model.kinematics().forward(q) };
                if (fk) {
                    out << std::format("Commanded TCP: {:.4f}, {:.4f}, {:.4f} mm (joint interpolation; point "
                                       "moves may curve).\n",
                                       fk->position.x.get() * MILLIMETRES_PER_METRE,
                                       fk->position.y.get() * MILLIMETRES_PER_METRE,
                                       fk->position.z.get() * MILLIMETRES_PER_METRE);
                }
            }
            out << "Feedback: M1 encoder; M2/M3 driver INDEX estimates, not measured mechanical position.\n";
            return 0;
        }
        constexpr Arg AXIS{ .name = "axis"_fs, .description = "shoulder | elbow | z"_fs };
        constexpr Arg VALUE{ .name = "value"_fs,
                             .description = "degrees for shoulder/elbow; millimetres for Z"_fs };
        constexpr Arg ID{ .name = "id"_fs, .description = "Execution job ID"_fs };
        constexpr Flag SPEED{ .name = "speed"_fs,
                              .description = "Axis: deg/s or mm/s; robot: 0..1 factor, default .25"_fs,
                              .value_name = "value"_fs };
        constexpr Flag ACCEL{ .name = "accel"_fs,
                              .description =
                                "Axis: deg/s^2 or mm/s^2; robot: 0..1 factor; zero uses default"_fs,
                              .value_name = "value"_fs };
        constexpr Flag DECEL{ .name = "decel"_fs,
                              .description =
                                "Axis: deg/s^2 or mm/s^2; robot: 0..1 factor; zero uses default"_fs,
                              .value_name = "value"_fs };
        constexpr Flag JERK{ .name = "jerk"_fs,
                             .description =
                               "Axis: deg/s^3 or mm/s^3; robot: 0..1 factor; zero uses default"_fs,
                             .value_name = "value"_fs };
        constexpr Flag TIMEOUT{ .name = "timeout"_fs,
                                .description =
                                  "Seconds until deceleration is requested; zero selects default"_fs,
                                .value_name = "seconds"_fs };
        constexpr Arg A{ .name = "a"_fs, .description = "Shoulder deg or X mm"_fs };
        constexpr Arg B{ .name = "b"_fs, .description = "Elbow deg or Y mm"_fs };
        constexpr Arg C{ .name = "c"_fs, .description = "Z mm"_fs };
    }
    namespace commands
    {
        [[
            = "Initialize drivers while disabled; does not enable"_fs,
            = Name{ "motion init"_fs }
        ]] static CallbackResult init(Context& c, const Arguments& /*unused*/, std::ostream& out)
        {
            c.client.get().request({ .operation = ::motion::MotorOperation::Initialize }, c.stop);
            out << "Initialized; drivers remain disabled.\n";
            return 0;
        }
        [[ = "Enable initialized drivers"_fs,
           = Name{ "motion enable"_fs } ]] static CallbackResult enable(Context& c,
                                                                        const Arguments& /*unused*/,
                                                                        std::ostream& out)
        {
            c.client.get().request({ .operation = ::motion::MotorOperation::Enable }, c.stop);
            out << "Drivers enabled.\n";
            return 0;
        }
        [[
            = "Abort, disable torque and invalidate references"_fs,
            = Name{ "motion disable"_fs }
        ]] static CallbackResult disable(Context& c, const Arguments& /*unused*/, std::ostream& out)
        {
            c.client.get().request({ .operation = ::motion::MotorOperation::Disable }, c.stop);
            out << "Drivers disabled.\n";
            return 0;
        }
        [[
            = "Readiness, ownership and configuration"_fs,
            = Name{ "motion status"_fs }
        ]] static CallbackResult status(Context& c, const Arguments& /*unused*/, std::ostream& out)
        {
            const auto s{ c.client.get().state() };
            out << "ready=" << s.ready << " enabled=" << s.enabled << " family=" << family(s.family)
                << " owners=" << s.owners[0] << ',' << s.owners[1] << ',' << s.owners[2]
                << " configuration=" << s.configuration << '\n';
            const auto lease{ c.client.get().leaseStatus() };
            out << "lease acknowledgements=" << lease.acknowledgements << " failures=" << lease.failures
                << " last error=" << ::motion::trajectory::name(lease.error);
            if (lease.transport_error) {
                out << " service error=" << unsigned(*lease.transport_error);
            }
            if (lease.last_acknowledgement) {
                out << " age="
                    << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - *lease.last_acknowledgement)
                         .count()
                    << " ms";
            }
            out << " (diagnostic only; IO owns lease expiry)\n";
            return 0;
        }
        [[
            = "Recent 32 execution jobs"_fs,
            = Name{ "motion jobs"_fs },
            = Arg{ .name = "id"_fs, .description = "Optional job ID"_fs, .optional = true }
        ]] static CallbackResult jobs(Context& c, const Arguments& a, std::ostream& out)
        {
            const auto input{ a.get("id") };
            const auto id{ input ? identifier(*input) : 0 };
            bool found{};
            for (const auto& j : c.client.get().jobs()) {
                if (!id || j.id == id) {
                    print_job(j, out);
                    found = true;
                }
            }
            if (id && !found) {
                return callback_failure("job not found");
            }
            return 0;
        }
        [[
            = "Wait for completion; timeout/interruption leaves motion running"_fs,
            = Name{ "motion wait"_fs },
            = ID,
            = Flag{ .name = "timeout"_fs,
                    .description =
                      "Maximum wait in seconds; zero waits indefinitely without cancelling motion"_fs,
                    .value_name = "seconds"_fs }
        ]] static CallbackResult wait(Context& c, const Arguments& a, std::ostream& out)
        {
            const auto seconds{ option(a, "timeout") };
            if (seconds > MAX_WAIT_SECONDS) {
                return callback_failure("wait timeout must be at most 86400 seconds");
            }
            print_job(
              c.client.get().wait(identifier(a.require("id")),
                                  std::chrono::milliseconds(std::int64_t(seconds * MILLISECONDS_PER_SECOND)),
                                  c.stop),
              out);
            return 0;
        }
        [[
            = "Cancel preparation or request controlled stopping"_fs,
            = Name{ "motion cancel"_fs },
            = ID
        ]] static CallbackResult cancel(Context& c, const Arguments& a, std::ostream& out)
        {
            c.client.get().cancel(identifier(a.require("id")));
            out << "Cancellation requested; use motion wait to confirm completion.\n";
            return 0;
        }
        [[
            = "Controlled stop of axis/robot jobs; raw jobs abort; torque retained"_fs,
            = Name{ "motion stop"_fs }
        ]] static CallbackResult stop(Context& c, const Arguments& /*unused*/, std::ostream& out)
        {
            c.client.get().stop();
            out << "Stopping requested; holding torque retained.\n";
            return 0;
        }
        [[
            = "Immediate cessation; affected references invalidated"_fs,
            = Name{ "motion abort"_fs }
        ]] static CallbackResult abort(Context& c, const Arguments& /*unused*/, std::ostream& out)
        {
            c.client.get().stop(::motion::ALL_AXES_MASK, true);
            out << "Abort requested; references invalidated.\n";
            return 0;
        }
        [[
            = "Joint coordinates and limits"_fs,
            = Name{ "axis status"_fs },
            = Arg{ .name = "axis"_fs, .description = "shoulder | elbow | z | all"_fs, .optional = true }
        ]] static CallbackResult axis_status(Context& c, const Arguments& a, std::ostream& out)
        {
            return positions(c, a, out, false);
        }
        [[
            = "Relative joint move; omitted/zero dynamics use configured defaults"_fs,
            = Name{ "axis move"_fs },
            = AXIS,
            = VALUE,
            = SPEED,
            = ACCEL,
            = DECEL,
            = JERK,
            = TIMEOUT
        ]] static CallbackResult axis_relative(Context& c, const Arguments& a, std::ostream& out)
        {
            return axis_move(c, a, out, true);
        }
        [[
            = "Absolute joint move; requires reference"_fs,
            = Name{ "axis moveto"_fs },
            = AXIS,
            = VALUE,
            = SPEED,
            = ACCEL,
            = DECEL,
            = JERK,
            = TIMEOUT
        ]] static CallbackResult axis_absolute(Context& c, const Arguments& a, std::ostream& out)
        {
            return axis_move(c, a, out, false);
        }
        [[
            = "Jog in deg/s or mm/s; default 1 second before deceleration"_fs,
            = Name{ "axis jog"_fs },
            = AXIS,
            = Arg{ .name = "speed"_fs, .description = "Signed joint speed: deg/s for rotary, mm/s for Z"_fs },
            = Flag{ .name = "duration"_fs,
                    .description = "Seconds until deceleration is requested"_fs,
                    .value_name = "seconds"_fs },
            = Flag{ .name = "continuous"_fs,
                    .description = "Until stopped, soft boundary, fault or owner lease expiry"_fs }
        ]] static CallbackResult axis_jog(Context& c, const Arguments& a, std::ostream& out)
        {
            const auto i{ axis(a.require("axis")) };
            Request r{};
            r.family = ::motion::Family::Axis;
            r.axes = 1U << i;
            r.kind = Request::Kind::Jog;
            r.speed = number(a.require("speed")) *
                      (i == 2 ? METRES_PER_MILLIMETRE : std::numbers::pi / DEGREES_PER_HALF_TURN);
            r.continuous = a.flagCount("continuous") != 0;
            r.duration = option(a, "duration", 1);
            if (a.flagCount("continuous") > 1 || (r.continuous && a.flagCount("duration"))) {
                return callback_failure("choose --duration or --continuous");
            }
            if (!r.speed || (!r.continuous && r.duration <= 0)) {
                return callback_failure("nonzero speed and positive duration required");
            }
            return submit(c, r, out);
        }
        [[
            = "Straight Cartesian jog; one path and local deceleration, default 1 second"_fs,
            = Name{ "robot jog"_fs },
            = Arg{ .name = "direction"_fs, .description = "x+, x-, y+, y-, z+ or z- in the base frame"_fs },
            = Flag{ .name = "speed"_fs,
                    .description = "Configured feed fraction (0,1], default .1"_fs,
                    .value_name = "factor"_fs },
            = Flag{ .name = "duration"_fs,
                    .description = "Seconds before local deceleration"_fs,
                    .value_name = "seconds"_fs },
            = Flag{ .name = "continuous"_fs,
                    .description = "Until stop, certified path boundary or lease expiry"_fs }
        ]] static CallbackResult cartesian_jog(Context& c, const Arguments& a, std::ostream& out)
        {
            const auto direction{ a.require("direction") };
            if (direction.size() != 2 ||
                (direction[0] != 'x' && direction[0] != 'y' && direction[0] != 'z') ||
                (direction[1] != '+' && direction[1] != '-')) {
                return callback_failure("direction must be x+, x-, y+, y-, z+ or z-");
            }
            Request r{};
            r.kind = Request::Kind::CartesianJog;
            r.direction[direction[0] - 'x'] = direction[1] == '+' ? 1. : -1.;
            r.dynamics.speed = option(a, "speed", DEFAULT_JOG_FACTOR);
            r.duration = option(a, "duration", 1);
            r.continuous = a.hasFlag("continuous");
            if (a.flagCount("continuous") > 1 || (r.continuous && a.hasFlag("duration"))) {
                return callback_failure("choose --duration or --continuous");
            }
            if (r.dynamics.speed <= 0 || r.dynamics.speed > 1 || (!r.continuous && r.duration <= 0)) {
                return callback_failure("speed factor must be (0,1]; duration must be positive");
            }
            return submit(c, r, out);
        }
        [[
            = "Seek, release and slow re-latch the axis"_fs,
            = Name{ "axis home"_fs },
            = AXIS
        ]] static CallbackResult axis_home(Context& c, const Arguments& a, std::ostream& out)
        {
            Request r{};
            r.family = ::motion::Family::Axis;
            r.axes = 1U << axis(a.require("axis"));
            r.kind = Request::Kind::Home;
            return submit(c, r, out);
        }
        [[
            = "Sequential configured homing with exclusive whole-robot ownership"_fs,
            = Name{ "robot home"_fs }
        ]] static CallbackResult robot_home(Context& c, const Arguments& /*unused*/, std::ostream& out)
        {
            Request r{};
            r.kind = Request::Kind::Home;
            return submit(c, r, out);
        }
        [[
            = "Controlled stop; a member of a robot job stops the whole group"_fs,
            = Name{ "axis stop"_fs },
            = AXIS
        ]] static CallbackResult axis_stop(Context& c, const Arguments& a, std::ostream& out)
        {
            c.robot.get().stopAxis(axis(a.require("axis")));
            out << "Stopping requested.\n";
            return 0;
        }
        [[
            = "Commanded joint/TCP state and feedback provenance"_fs,
            = Name{ "robot status"_fs }
        ]] static CallbackResult robot_status(Context& c, const Arguments& a, std::ostream& out)
        {
            return positions(c, a, out, true);
        }
        [[
            = "Controlled stop of the owning robot group"_fs,
            = Name{ "robot stop"_fs }
        ]] static CallbackResult robot_stop(Context& c, const Arguments& /*unused*/, std::ostream& out)
        {
            c.robot.get().stop();
            out << "Robot stop requested.\n";
            return 0;
        }
        [[
            = "Relative synchronized joints; speed/accel/decel/jerk are configured-limit factors (0..1)"_fs,
            = Name{ "robot move joints"_fs },
            = A,
            = B,
            = C,
            = SPEED,
            = ACCEL,
            = DECEL,
            = JERK,
            = TIMEOUT
        ]] static CallbackResult relative_joints(Context& c, const Arguments& a, std::ostream& out)
        {
            return robot_move(c, a, out, true, false);
        }
        [[
            = "Absolute synchronized joints; speed factor defaults to .25"_fs,
            = Name{ "robot moveto joints"_fs },
            = A,
            = B,
            = C,
            = SPEED,
            = ACCEL,
            = DECEL,
            = JERK,
            = TIMEOUT
        ]] static CallbackResult absolute_joints(Context& c, const Arguments& a, std::ostream& out)
        {
            return robot_move(c, a, out, false, false);
        }
        [[
            = "Relative Cartesian destination via joint interpolation; TCP path can curve"_fs,
            = Name{ "robot move point"_fs },
            = A,
            = B,
            = C,
            = SPEED,
            = ACCEL,
            = DECEL,
            = JERK,
            = TIMEOUT
        ]] static CallbackResult relative_point(Context& c, const Arguments& a, std::ostream& out)
        {
            return robot_move(c, a, out, true, true);
        }
        [[
            = "Absolute Cartesian destination via joint interpolation; TCP path can curve"_fs,
            = Name{ "robot moveto point"_fs },
            = A,
            = B,
            = C,
            = SPEED,
            = ACCEL,
            = DECEL,
            = JERK,
            = TIMEOUT
        ]] static CallbackResult absolute_point(Context& c, const Arguments& a, std::ostream& out)
        {
            return robot_move(c, a, out, false, true);
        }
    }
    void setup(Parser& parser, Client& client, Robot& robot, std::stop_token stop)
    {
        parser.registerModule(Parser::Module::Motion, [&] {
            register_commands<^^commands>(
              parser, [context = Context{ .client = client, .robot = robot, .stop = stop }](auto command) {
                return [context, command](const Arguments& a, std::ostream& out) mutable {
                    return command(context, a, out);
                };
            });
        });
    }
}
