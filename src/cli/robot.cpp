#include "robot.hpp"
#include "control_common.hpp"

#include <charconv>
#include <format>
#include <ostream>

namespace cli::robot
{
    namespace
    {
        using Kinematics = ScaraKinematics;
        using namespace pnm::units;
        using namespace control_detail;
        constexpr auto degrees_per_second{ 1_deg / 1_s };

        std::string_view errorName(Kinematics::Error error)
        {
            using enum Kinematics::Error;
            switch (error) {
                case InvalidInput:
                    return "invalid input";
                case Unreachable:
                    return "target is outside the reachable workspace";
                case JointLimitExceeded:
                    return "joint limit exceeded";
                case Singularity:
                    return "singularity (straight/folded arm or ambiguous elbow branch)";
                case NumericOverflow:
                    return "numeric overflow";
            }
            std::unreachable();
        }

        void printPose(std::ostream& out, const Kinematics::ToolPose& pose)
        {
            out << std::format("Tool: x={:.4f} mm  y={:.4f} mm  z={:.4f} mm  yaw={:.4f} deg\n",
                               pose.position.x / 1_mm,
                               pose.position.y / 1_mm,
                               pose.position.z / 1_mm,
                               pose.yaw / 1_deg);
        }

        void printJoints(std::ostream& out, Kinematics::JointPosition joints)
        {
            out << std::format("Joints: shoulder={:.4f} deg  elbow={:.4f} deg  z={:.4f} mm\n",
                               joints.shoulder / 1_deg,
                               joints.elbow / 1_deg,
                               joints.z / 1_mm);
        }

        void printState(std::ostream& out, const control::Robot::State& state)
        {
            printJoints(out, state.joints);
            if (state.pose)
                printPose(out, *state.pose);
            else
                out << "Tool: unavailable (" << errorName(state.pose.error()) << ")\n";
            if (state.velocity) {
                const auto& speed{ *state.velocity };
                out << std::format("Tool velocity: x={:.3f} mm/s  y={:.3f} mm/s  z={:.3f} mm/s  "
                                   "yaw={:.3f} deg/s\n",
                                   speed.linear.x / 1_mm_s,
                                   speed.linear.y / 1_mm_s,
                                   speed.linear.z / 1_mm_s,
                                   speed.yaw / degrees_per_second);
            }
            else
                out << "Tool velocity: unavailable (" << errorName(state.velocity.error()) << ")\n";
        }

        std::optional<Kinematics::ElbowBranch> branch(const Arguments& args)
        {
            const auto value{ option(args, "branch").value_or("current") };
            if (value == "current")
                return {};
            if (value == "positive")
                return Kinematics::ElbowBranch::Positive;
            if (value == "negative")
                return Kinematics::ElbowBranch::Negative;
            throw std::invalid_argument("--branch must be current, positive or negative");
        }
        control::Robot::MoveOptions options(const Arguments& args, bool cartesian)
        {
            const auto speed{ numericOption(args, "speed", 20, false) };
            if (speed > 100)
                throw std::invalid_argument("--speed must be greater than 0 and at most 100 percent");
            return { speed / 100,
                     numericOption(args, "timeout", 0) * 1_s,
                     cartesian ? branch(args) : std::nullopt };
        }

        int submitted(const control::Robot::Motion& motion, std::ostream& out)
        {
            out << std::format("Robot #{} submitted", motion.id);
            if (motion.duration > 0_s)
                out << std::format(" (planned duration {:.3f} s)", motion.duration / 1_s);
            out << std::format(". Check completion with: robot jobs {}\n", motion.id);
            return 0;
        }

        std::string_view resultName(std::optional<StepperMotor::Result> result)
        {
            if (!result)
                return "outstanding";
            using enum StepperMotor::Result;
            switch (*result) {
                case Completed:
                    return "completed";
                case Stopped:
                    return "stopped";
                case TimedOut:
                    return "timed-out";
                case Rejected:
                    return "rejected";
                case Faulted:
                    return "faulted";
            }
            std::unreachable();
        }
    }

    namespace commands
    {
        using namespace pnm::meta::string::literals;

        [[ = "SCARA motion, homing, state and kinematics calculations"_fs,
           = Name{ "robot"_fs } ]] static auto overview(control::Robot&, const Arguments&, std::ostream& out)
          -> CallbackResult
        {
            out << "Robot control (distances in mm, joint angles in degrees):\n"
                   "  robot status                  Live commanded tool position and velocity\n"
                   "  robot geometry                Configured arm lengths, frames and limits\n"
                   "  robot fk <shoulder> <elbow> <z>    Joint coordinates to tool pose\n"
                   "  robot ik <x> <y> <z>          Tool position to joint coordinates\n"
                   "    --branch current|positive|negative (default: current)\n"
                   "  robot enable / disable / stop / reset     Shared driver and motion control\n"
                   "  robot home [--timeout seconds]            Reference Z, shoulder, then elbow\n"
                   "  robot moveto <x> <y> <z>                   Absolute Cartesian target\n"
                   "  robot move <dx> <dy> <dz>                  Relative Cartesian target\n"
                   "  robot joints <shoulder> <elbow> <z>        Absolute joint target\n"
                   "    --speed percent (default 20), --timeout seconds (default unlimited)\n"
                   "    Cartesian moves also accept --branch current|positive|negative.\n"
                   "  robot jobs [id]                            Operation completion/results\n"
                   "Moves are synchronized point-to-point joint motion; tool paths can curve.\n"
                   "FK/IK calculations do not move the robot. All moves require referenced, idle axes.\n";
            return 0;
        }

        [[
            = "Live tool pose/velocity from axis coordinates, with reference and feedback validity"_fs,
            = Name{ "robot status"_fs }
        ]] static auto status(control::Robot& robot, const Arguments&, std::ostream& out) -> CallbackResult
        {
            const auto status{ robot.status() };
            out << std::format("Drivers: {} | timebase: {} | pulse counts: {} | referenced={}\n",
                               status.motors.enabled ? "enabled" : "disabled",
                               stateName(status.motors.generator.state),
                               status.motors.generator.counts_exact ? "exact" : "uncertain",
                               status.referenced ? "yes" : "no");
            out << "Robot operation: " << (status.motors.coordinated ? "active" : "idle") << '\n';
            if (!status.referenced)
                out << "Unreferenced coordinates have no established physical datum.\n";
            out << "Commanded state (from emitted steps):\n";
            printState(out, status.commanded);
            if (status.actual) {
                out << "Measured state (all three encoders):\n";
                printState(out, *status.actual);
            }
            else {
                const auto& error{ status.actual.error() };
                out << "Measured tool state unavailable: " << axisName(error.motor) << " encoder ";
                if (error.reason == std::errc::no_such_device)
                    out << "not fitted";
                else
                    out << "unavailable (" << error.reason.message() << ')';
                out << ". See 'axis status' for individual feedback.\n";
            }
            return 0;
        }

        [[
            = "Show configured arm lengths, base/tool frames and joint limits"_fs,
            = Name{ "robot geometry"_fs }
        ]] static auto geometry(control::Robot& robot, const Arguments&, std::ostream& out) -> CallbackResult
        {
            const auto& config{ robot.kinematics().configuration() };
            out << std::format("Arms: first={:.4f} mm  second={:.4f} mm\n",
                               config.first_arm_length / 1_mm,
                               config.second_arm_length / 1_mm);
            out << std::format("Base: x={:.4f} mm  y={:.4f} mm  z={:.4f} mm  yaw={:.4f} deg\n",
                               config.base_origin.x / 1_mm,
                               config.base_origin.y / 1_mm,
                               config.base_origin.z / 1_mm,
                               config.base_yaw / 1_deg);
            out << std::format("Tool offset: x={:.4f} mm  y={:.4f} mm  z={:.4f} mm  yaw={:.4f} deg\n",
                               config.tool_offset.x / 1_mm,
                               config.tool_offset.y / 1_mm,
                               config.tool_offset.z / 1_mm,
                               config.tool_yaw / 1_deg);
            if (config.limits) {
                out << "Minimum ";
                printJoints(out, config.limits->minimum);
                out << "Maximum ";
                printJoints(out, config.limits->maximum);
            }
            else
                out << "Joint limits: not configured\n";
            out << std::format("Minimum bend sine: {:g}\n", config.minimum_bend_sine);
            const auto limits{ robot.maximumVelocity() };
            out << std::format(
              "Joint speed limits: shoulder={:.3f} deg/s  elbow={:.3f} deg/s  z={:.3f} mm/s\n",
              limits.shoulder / degrees_per_second,
              limits.elbow / degrees_per_second,
              limits.z / 1_mm_s);
            return 0;
        }

        [[
            = "Calculate tool pose from shoulder/elbow angles and Z; does not move"_fs,
            = Name{ "robot fk"_fs },
            = Arg{ .name = "shoulder"_fs, .description = "Shoulder angle (degrees)"_fs },
            = Arg{ .name = "elbow"_fs, .description = "Elbow angle relative to first arm (degrees)"_fs },
            = Arg{ .name = "z"_fs, .description = "Z carriage coordinate (mm)"_fs }
        ]] static auto fk(control::Robot& robot, const Arguments& args, std::ostream& out) -> CallbackResult
        {
            const auto pose{ robot.kinematics().forward(
              { number(args.require("shoulder"), "shoulder") * 1_deg,
                number(args.require("elbow"), "elbow") * 1_deg,
                number(args.require("z"), "z") * 1_mm }) };
            if (!pose)
                return callback_failure(std::string(errorName(pose.error())));
            printPose(out, *pose);
            return 0;
        }

        [[
            = "Calculate a joint endpoint using current commanded joints as seed; does not move"_fs,
            = Name{ "robot ik"_fs },
            = Arg{ .name = "x"_fs, .description = "Tool X in world frame (mm)"_fs },
            = Arg{ .name = "y"_fs, .description = "Tool Y in world frame (mm)"_fs },
            = Arg{ .name = "z"_fs, .description = "Tool Z in world frame (mm)"_fs },
            = Flag{ .name = "branch"_fs,
                    .description = "Elbow branch: current (default), positive, negative"_fs,
                    .value_name = "name"_fs }
        ]] static auto ik(control::Robot& robot, const Arguments& args, std::ostream& out) -> CallbackResult
        {
            const Kinematics::CartesianPosition target{ number(args.require("x"), "x") * 1_mm,
                                                        number(args.require("y"), "y") * 1_mm,
                                                        number(args.require("z"), "z") * 1_mm };
            const auto selected_branch{ branch(args) };
            const auto status{ robot.status() };
            const auto result{ robot.kinematics().inverse(target, status.commanded.joints, selected_branch) };
            if (!result)
                return callback_failure(std::string(errorName(result.error())));
            printJoints(out, *result);
            out << "Endpoint calculation only; no motion started or path validated.\n";
            if (!status.referenced)
                out << "Seed is unreferenced; joint coordinates have no established physical datum.\n";
            return 0;
        }
        [[ = "Enable all drivers and wait for settling"_fs,
           = Name{ "robot enable"_fs } ]] static auto enable(control::Robot& robot,
                                                             const Arguments&,
                                                             std::ostream& out) -> CallbackResult
        {
            robot.enable();
            out << "All drivers enabled and settled.\n";
            return 0;
        }

        [[
            = "Abort robot/motor motions, disable drivers and invalidate references"_fs,
            = Name{ "robot disable"_fs }
        ]] static auto disable(control::Robot& robot, const Arguments&, std::ostream& out) -> CallbackResult
        {
            robot.disable();
            out << "All drivers disabled; references invalidated.\n";
            return 0;
        }

        [[
            = "Abort the whole robot operation immediately; keep holding torque"_fs,
            = Name{ "robot stop"_fs }
        ]] static auto stop(control::Robot& robot, const Arguments&, std::ostream& out) -> CallbackResult
        {
            robot.stop();
            out << "Robot motion stopped; driver enable unchanged.\n";
            return 0;
        }

        [[
            = "Recover shared timebase while drivers are disabled"_fs,
            = Name{ "robot reset"_fs }
        ]] static auto reset(control::Robot& robot, const Arguments&, std::ostream& out) -> CallbackResult
        {
            robot.reset();
            out << "Timebase restarted; drivers remain disabled.\n";
            return 0;
        }

        [[
            = "Reference all axes sequentially: Z, shoulder, elbow; requires idle axes"_fs,
            = Name{ "robot home"_fs },
            = Flag{ .name = "timeout"_fs,
                    .description = "Overall homing timeout, default 90 (>0)"_fs,
                    .value_name = "seconds"_fs }
        ]] static auto home(control::Robot& robot, const Arguments& args, std::ostream& out) -> CallbackResult
        {
            return submitted(robot.reference(numericOption(args, "timeout", 90, false) * 1_s), out);
        }

        [[
            = "Synchronized joint PTP to Cartesian XYZ; tool path may curve"_fs,
            = Name{ "robot moveto"_fs },
            = Arg{ .name = "x"_fs, .description = "World x (mm)"_fs },
            = Arg{ .name = "y"_fs, .description = "World y (mm)"_fs },
            = Arg{ .name = "z"_fs, .description = "World z (mm)"_fs },
            = Flag{ .name = "speed"_fs,
                    .description = "Fraction of configured joint speed limits (0..100, default 20)"_fs,
                    .value_name = "percent"_fs },
            = Flag{ .name = "timeout"_fs,
                    .description = "Overall execution timeout; 0 means unlimited"_fs,
                    .value_name = "seconds"_fs },
            = Flag{ .name = "branch"_fs,
                    .description =
                      "current (default), positive, negative; choose explicitly at singular home"_fs,
                    .value_name = "name"_fs }
        ]] static auto moveto(control::Robot& robot, const Arguments& args, std::ostream& out)
          -> CallbackResult
        {
            const Kinematics::CartesianPosition target{ number(args.require("x"), "x") * 1_mm,
                                                        number(args.require("y"), "y") * 1_mm,
                                                        number(args.require("z"), "z") * 1_mm };
            return submitted(robot.moveAbs(target, options(args, true)), out);
        }

        [[
            = "Synchronized joint PTP by Cartesian displacement; tool path may curve"_fs,
            = Name{ "robot move"_fs },
            = Arg{ .name = "dx"_fs, .description = "World dx (mm)"_fs },
            = Arg{ .name = "dy"_fs, .description = "World dy (mm)"_fs },
            = Arg{ .name = "dz"_fs, .description = "World dz (mm)"_fs },
            = Flag{ .name = "speed"_fs,
                    .description = "Fraction of configured joint speed limits (0..100, default 20)"_fs,
                    .value_name = "percent"_fs },
            = Flag{ .name = "timeout"_fs,
                    .description = "Overall execution timeout; 0 means unlimited"_fs,
                    .value_name = "seconds"_fs },
            = Flag{ .name = "branch"_fs,
                    .description =
                      "current (default), positive, negative; choose explicitly at singular home"_fs,
                    .value_name = "name"_fs }
        ]] static auto move(control::Robot& robot, const Arguments& args, std::ostream& out) -> CallbackResult
        {
            const Kinematics::CartesianPosition target{ number(args.require("dx"), "dx") * 1_mm,
                                                        number(args.require("dy"), "dy") * 1_mm,
                                                        number(args.require("dz"), "dz") * 1_mm };
            return submitted(robot.moveRel(target, options(args, true)), out);
        }

        [[
            = "Synchronized PTP to shoulder/elbow degrees and Z mm; reference required"_fs,
            = Name{ "robot joints"_fs },
            = Arg{ .name = "shoulder"_fs, .description = "Shoulder angle (degrees)"_fs },
            = Arg{ .name = "elbow"_fs, .description = "Relative elbow angle (degrees)"_fs },
            = Arg{ .name = "z"_fs, .description = "Z carriage coordinate (mm)"_fs },
            = Flag{ .name = "speed"_fs,
                    .description = "Fraction of configured joint speed limits (0..100, default 20)"_fs,
                    .value_name = "percent"_fs },
            = Flag{ .name = "timeout"_fs,
                    .description = "Overall execution timeout; 0 means unlimited"_fs,
                    .value_name = "seconds"_fs }
        ]] static auto joints(control::Robot& robot, const Arguments& args, std::ostream& out)
          -> CallbackResult
        {
            const Kinematics::JointPosition target{ number(args.require("shoulder"), "shoulder") * 1_deg,
                                                    number(args.require("elbow"), "elbow") * 1_deg,
                                                    number(args.require("z"), "z") * 1_mm };
            return submitted(robot.moveJoints(target, options(args, false)), out);
        }

        [[
            = "Most recent 32 robot operation results; retained handles remain valid"_fs,
            = Name{ "robot jobs"_fs },
            = Arg{ .name = "id"_fs, .description = "Optional robot operation ID"_fs, .optional = true }
        ]] static auto jobs(control::Robot& robot, const Arguments& args, std::ostream& out) -> CallbackResult
        {
            std::optional<control::MotionController::MotionId> selected;
            if (const auto text{ args.get("id") }) {
                control::MotionController::MotionId id{};
                const auto parsed{ std::from_chars(text->data(), text->data() + text->size(), id) };
                if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size() || id == 0)
                    return callback_failure("id must be a positive integer");
                selected = id;
            }
            bool found{};
            for (const auto& job : robot.motions()) {
                if (selected && *selected != job.id)
                    continue;
                found = true;
                out << std::format("Robot #{} {} {}\n",
                                   job.id,
                                   job.operation == control::MotionController::Operation::Reference ? "home"
                                                                                                    : "move",
                                   resultName(job.result));
                if (selected && job.result && *job.result != StepperMotor::Result::Completed)
                    return 1;
            }
            if (!found && selected)
                return callback_failure("unknown or expired robot operation ID");
            if (!found)
                out << "No robot operations submitted.\n";
            return 0;
        }

    }

    void setup(Parser& parser, std::shared_ptr<control::Robot> robot)
    {
        if (!robot)
            throw std::invalid_argument("robot commands require a robot");
        register_commands(parser, [robot](auto command) {
            return [robot, command](const Arguments& args, std::ostream& out) {
                return command(*robot, args, out);
            };
        });
    }
}
