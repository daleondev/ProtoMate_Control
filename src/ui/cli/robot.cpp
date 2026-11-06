#include "robot.hpp"
#include "Parser.hpp"
#include "ui/motion/Robot.hpp"
#include <charconv>
#include <cmath>
#include <format>
#include <ostream>
#include <stdexcept>

namespace ui::cli::robot
{
    using namespace pnm::meta::string::literals;
    namespace
    {
        using namespace pnm::units;
        using namespace pnm::units::literals;
        using ui::motion::Robot;
        double number(std::string_view text)
        {
            double value{};
            const auto parsed{ std::from_chars(text.data(), text.data() + text.size(), value) };
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                !std::isfinite(value)) {
                throw std::invalid_argument("expected a finite number");
            }
            return value;
        }
        std::uint64_t identifier(std::string_view text)
        {
            std::uint64_t value{};
            const auto parsed{ std::from_chars(text.data(), text.data() + text.size(), value) };
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !value) {
                throw std::invalid_argument("invalid job ID");
            }
            return value;
        }
        const char* state(Robot::JobState s)
        {
            switch (s) {
                case Robot::JobState::Queued:
                    return "queued";
                case Robot::JobState::Planning:
                    return "planning";
                case Robot::JobState::Preview:
                    return "preview ready (no motor execution)";
                case Robot::JobState::Rejected:
                    return "rejected";
                case Robot::JobState::Cancelled:
                    return "cancelled";
                case Robot::JobState::Unknown:
                    return "unknown / disconnected";
                default:
                    return "empty";
            }
        }
        CallbackResult preview(Robot& robot,
                               const Arguments& args,
                               std::ostream& out,
                               ui::motion::MoveKind kind)
        {
            Robot::PreviewRequest request{};
            request.move.kind = kind;
            const auto fixture{ args.flagValue("fixture") };
            if (fixture) {
                if (*fixture == "180") {
                    request.model = Robot::Model::Fixture180;
                }
                else if (*fixture == "archived") {
                    request.model = Robot::Model::FixtureArchived;
                }
                else {
                    return callback_failure("fixture must be 180 or archived; neither is a calibrated robot");
                }
            }
            for (const auto* name : { "start-shoulder", "start-elbow", "start-z" }) {
                if (args.flagCount(name) != 1 || !args.flagValue(name)) {
                    return callback_failure(std::string("--") + name + " is required exactly once");
                }
            }
            request.start = { .shoulder = number(*args.flagValue("start-shoulder")) * 1_deg,
                              .elbow = number(*args.flagValue("start-elbow")) * 1_deg,
                              .z = number(*args.flagValue("start-z")) * 1_mm };
            const auto a{ number(args.require("a")) };
            const auto b{ number(args.require("b")) };
            const auto c{ number(args.require("c")) };
            if (kind == ui::motion::MoveKind::Joints) {
                request.move.joints = { .shoulder = a * 1_deg, .elbow = b * 1_deg, .z = c * 1_mm };
            }
            else {
                request.move.target = { .x = a * 1_mm, .y = b * 1_mm, .z = c * 1_mm };
            }
            if (const auto speed{ args.flagValue("speed") }) {
                request.move.speed = number(*speed);
            }
            if (const auto feed{ args.flagValue("feed") }) {
                request.move.feed = number(*feed) * 1_mm_s;
            }
            if (args.hasFlag("work")) {
                request.move.frame = ui::motion::CoordinateFrame::Work;
            }
            const auto job{ robot.preview(request) };
            if (!job) {
                return callback_failure(std::string(::motion::trajectory::name(job.error())));
            }
            out << "Queued preview " << *job << "; use 'robot jobs " << *job
                << "'. Motors remain unchanged.\n";
            return 0;
        }
        constexpr Flag FIXTURE{ .name = "fixture"_fs,
                                .description = "Uncalibrated simulation model: 180 or archived"_fs,
                                .value_name = "name"_fs };
        constexpr Flag Q1{ .name = "start-shoulder"_fs,
                           .description = "Explicit starting shoulder angle"_fs,
                           .value_name = "degrees"_fs };
        constexpr Flag Q2{ .name = "start-elbow"_fs,
                           .description = "Explicit starting elbow angle"_fs,
                           .value_name = "degrees"_fs };
        constexpr Flag Z{ .name = "start-z"_fs,
                          .description = "Explicit starting Z position"_fs,
                          .value_name = "mm"_fs };
        constexpr Flag SPEED{ .name = "speed"_fs,
                              .description = "Requested speed fraction (0 < value <= 1)"_fs,
                              .value_name = "fraction"_fs };
        constexpr Flag FEED{ .name = "feed"_fs,
                             .description = "TCP feed, limited by configuration"_fs,
                             .value_name = "mm-s"_fs };
        constexpr Flag WORK{ .name = "work"_fs,
                             .description = "Cartesian target is in the configured work frame"_fs };
        constexpr Arg A{ .name = "a"_fs, .description = "Shoulder degrees or Cartesian X mm"_fs };
        constexpr Arg B{ .name = "b"_fs, .description = "Elbow degrees or Cartesian Y mm"_fs };
        constexpr Arg C{ .name = "c"_fs, .description = "Z mm"_fs };
    }
    namespace commands
    {
        [[
            = "Robot execution, planning previews and configuration diagnostics"_fs,
            = Name{ "robot"_fs }
        ]] static CallbackResult overview(Robot&, const Arguments&, std::ostream& out)
        {
            out << "robot home / robot status / robot stop\n"
                   "robot move|moveto joints <shoulder-deg> <elbow-deg> <z-mm>\n"
                   "robot move|moveto point <x-mm> <y-mm> <z-mm>  Joint interpolation; TCP may curve\n"
                   "motion jobs [id] / motion wait <id> / motion cancel <id> track execution.\n"
                   "robot preview joints <shoulder-deg> <elbow-deg> <z-mm>\n"
                   "robot preview point <x-mm> <y-mm> <z-mm>   Joint move to Cartesian endpoint; TCP may "
                   "curve\n"
                   "robot preview linear <x-mm> <y-mm> <z-mm>  Straight TCP path\n"
                   "All previews require --start-shoulder <deg> --start-elbow <deg> --start-z <mm>.\n"
                   "--fixture 180|archived selects uncalibrated simulation geometry explicitly.\n"
                   "robot jobs [id] / robot cancel <id>\n"
                   "Executable moves require the native simulation model or a verified physical "
                   "configuration.\n";
            return 0;
        }
        [[
            = "Preview synchronized joint interpolation (degrees, degrees, mm)"_fs,
            = Name{ "robot preview joints"_fs },
            = A,
            = B,
            = C,
            = FIXTURE,
            = Q1,
            = Q2,
            = Z,
            = SPEED
        ]] static CallbackResult joints(Robot& robot, const Arguments& args, std::ostream& out)
        {
            return preview(robot, args, out, ui::motion::MoveKind::Joints);
        }
        [[
            = "Preview a Cartesian endpoint via joint interpolation; TCP path may curve"_fs,
            = Name{ "robot preview point"_fs },
            = A,
            = B,
            = C,
            = FIXTURE,
            = Q1,
            = Q2,
            = Z,
            = SPEED,
            = WORK
        ]] static CallbackResult point(Robot& robot, const Arguments& args, std::ostream& out)
        {
            return preview(robot, args, out, ui::motion::MoveKind::Point);
        }
        [[
            = "Preview a straight Cartesian path (X/Y/Z in mm)"_fs,
            = Name{ "robot preview linear"_fs },
            = A,
            = B,
            = C,
            = FIXTURE,
            = Q1,
            = Q2,
            = Z,
            = SPEED,
            = FEED,
            = WORK
        ]] static CallbackResult linear(Robot& robot, const Arguments& args, std::ostream& out)
        {
            return preview(robot, args, out, ui::motion::MoveKind::Linear);
        }
        [[
            = "Inspect bounded planning jobs; reports do not prove motor or TCP execution"_fs,
            = Name{ "robot jobs"_fs },
            = Arg{ .name = "id"_fs, .description = "Optional job ID"_fs, .optional = true }
        ]] static CallbackResult jobs(Robot& robot, const Arguments& args, std::ostream& out)
        {
            const auto selected{ args.get("id") ? identifier(*args.get("id")) : 0 };
            bool found{ false };
            for (const auto id : robot.jobs()) {
                if (!id || (selected && selected != id)) {
                    continue;
                }
                const auto job{ robot.job(id) };
                if (!job) {
                    continue;
                }
                found = true;
                out << std::format("{}: {} | {} | model={}\n",
                                   id,
                                   state(job->state),
                                   ::motion::trajectory::name(job->error),
                                   job->model == Robot::Model::Physical     ? "physical"
                                   : job->model == Robot::Model::Fixture180 ? "180 simulation"
                                                                            : "archived simulation");
                if (job->state != Robot::JobState::Preview) {
                    continue;
                }
                const auto& p{ job->plan };
                out << std::format(
                  "  {:.6f} s; {} / 512 segments; elbow={}; approximation bound={:.4f} mm; separate "
                  "quantization bound={:.4f} mm\n",
                  p.duration,
                  p.segments,
                  p.branch == ui::motion::ScaraKinematics::ElbowBranch::Positive ? "positive" : "negative",
                  p.path_error_bound * 1000,
                  p.quantization_bound * 1000);
                out << std::format(
                  "  endpoint: {:.5f} deg, {:.5f} deg, {:.5f} mm; TCP {:.5f}, {:.5f}, {:.5f} mm\n",
                  job->endpoint.shoulder.get<AngleUnits::deg>(),
                  job->endpoint.elbow.get<AngleUnits::deg>(),
                  job->endpoint.z.get() * 1000,
                  job->tcp.x.get() * 1000,
                  job->tcp.y.get() * 1000,
                  job->tcp.z.get() * 1000);
                for (unsigned i{ 0 }; i < 3; ++i) {
                    out << std::format("  m{} peaks <= {:.3f} steps/s, {:.3f} steps/s^2, {:.3f} steps/s^3; "
                                       "endpoint={} steps\n",
                                       i + 1,
                                       p.peak_velocity[i],
                                       p.peak_acceleration[i],
                                       p.peak_jerk[i],
                                       p.endpoint[i]);
                }
            }
            if (selected && !found) {
                return callback_failure("job not found (64 records retained)");
            }
            if (!found) {
                out << "No robot previews.\n";
            }
            return 0;
        }
        [[
            = "Cancel a pending preview"_fs,
            = Name{ "robot cancel"_fs },
            = Arg{ .name = "id"_fs, .description = "Job ID"_fs }
        ]] static CallbackResult cancel(Robot& robot, const Arguments& args, std::ostream& out)
        {
            const auto error{ robot.cancel(identifier(args.require("id"))) };
            if (error != ui::motion::RobotError::None) {
                return callback_failure(std::string(::motion::trajectory::name(error)));
            }
            out << "Preview cancelled.\n";
            return 0;
        }
    }
    void setup(Parser& parser, ui::motion::Robot& robot)
    {
        std::call_once(parser.m_robotRegistration, [&] {
            register_commands<^^commands>(parser, [&robot](auto command) {
                return [&robot, command](const Arguments& args, std::ostream& out) {
                    return command(robot, args, out);
                };
            });
        });
    }
}
