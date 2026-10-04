#include "motor.hpp"
#include "control_common.hpp"

#include <format>
#include <ostream>
#include <stdexcept>

namespace cli::motor
{
    namespace
    {
        using Controller = control::MotionController;
        using MotorId = Controller::MotorId;
        using namespace pnm::units;
        using namespace control_detail;

        constexpr std::string_view guide{
            "Motor control (motor-shaft degrees and rpm, including M3):\n"
            "  motor status [m1|m2|m3|all]          Positions, feedback and reference state\n"
            "  motor enable / motor disable        Shared driver enable\n"
            "  motor home <motor>                  Seek, release and slowly re-latch reference\n"
            "  motor move <motor> <degrees> --speed <rpm>    Relative move\n"
            "  motor moveto <motor> <degrees> --speed <rpm>  Absolute move (reference required)\n"
            "  motor speed <motor> <rpm>           Replan remaining motion at a new speed\n"
            "  motor stop [m1|m2|m3|all]            Immediate abort; retains holding torque\n"
            "  motor defaults <motor>              Show/change axis dynamics\n"
            "  motor jobs [id]                     Outstanding and recent motion results\n"
            "  motor reset                         Reset timebase while drivers are disabled\n"
            "Use 'help motor move', 'help motor home', etc. for options.\n"
            "Moves/home return a motion ID immediately; check its result with 'motor jobs <id>'.\n"
            "No axis is referenced automatically. Disabling invalidates all references.\n"
        };

        MotorId motorId(std::string_view text)
        {
            if (text == "m1" || text == "1")
                return MotorId::Motor1;
            if (text == "m2" || text == "2")
                return MotorId::Motor2;
            if (text == "m3" || text == "3")
                return MotorId::Motor3;
            throw std::invalid_argument("motor must be m1, m2 or m3 (also 1, 2, 3)");
        }

        std::optional<MotorId> selection(const Arguments& args)
        {
            const auto text{ args.get("motor") };
            return !text || *text == "all" ? std::nullopt : std::optional{ motorId(*text) };
        }

        auto performMove(Controller& controller, const Arguments& args, std::ostream& out, bool absolute)
          -> CallbackResult
        {
            const auto id{ motorId(args.require("motor")) };
            const auto speed{ option(args, "speed") };
            if (!speed)
                return callback_failure("--speed <rpm> is required");
            const Controller::Move request{ .position = number(args.require("degrees"), "degrees") * 1_deg,
                                            .velocity =
                                              magnitude(number(*speed, "speed"), "speed", false) * 1_rpm,
                                            .acceleration = numericOption(args, "accel", 0) * 1_deg_s2,
                                            .deceleration = numericOption(args, "decel", 0) * 1_deg_s2,
                                            .jerk = numericOption(args, "jerk", 0) * 1_deg_s3,
                                            .buffer = bufferMode(args),
                                            .timeout = numericOption(args, "timeout", 0) * 1_s,
                                            .absolute = absolute };
            return submitted(controller.move(id, request), out);
        }
    }

    namespace commands
    {
        using namespace pnm::meta::string::literals;

        [[
            = "Motor commands and examples; help motor <command> for details"_fs,
            = Name{ "motor"_fs }
        ]] static auto overview(Controller&, const Arguments&, std::ostream& out) -> CallbackResult
        {
            out << guide;
            return 0;
        }

        [[
            = "Live commanded/encoder values; all units refer to motor shafts"_fs,
            = Name{ "motor status"_fs },
            = Arg{ .name = "motor"_fs, .description = "m1, m2, m3 or all (default)"_fs, .optional = true }
        ]] static auto status(Controller& controller, const Arguments& args, std::ostream& out)
          -> CallbackResult
        {
            const auto selected{ selection(args) };
            const auto status{ controller.status() };
            out << std::format("Drivers: {} | timebase: {} | pulse counts: {}\n",
                               status.enabled ? "enabled" : "disabled",
                               stateName(status.generator.state),
                               status.generator.counts_exact ? "exact" : "uncertain");
            for (const auto& axis : status.axes) {
                if (selected && *selected != axis.motor)
                    continue;
                const auto i{ static_cast<std::size_t>(axis.motor) };
                out << std::format("m{}: position={:.4f} deg  velocity={:.3f} rpm  referenced={}  switch={}  "
                                   "outstanding={}  step={}\n",
                                   i + 1U,
                                   axis.position.get<AngleUnits::deg>(),
                                   axis.velocity.get<AngularVelocityUnits::rpm>(),
                                   axis.referenced ? "yes" : "no",
                                   axis.reference_switch_active ? "active/open" : "released",
                                   axis.outstanding,
                                   stateName(status.generator.axes[i]));
                if (axis.actual_position && axis.actual_velocity)
                    out << std::format("    encoder: position={:.4f} deg  velocity={:.3f} rpm\n",
                                       axis.actual_position->get<AngleUnits::deg>(),
                                       axis.actual_velocity->get<AngularVelocityUnits::rpm>());
                else if (!axis.actual_position && axis.actual_position.error() == std::errc::no_such_device)
                    out << "    encoder: not fitted\n";
                else
                    out << "    encoder: unavailable ("
                        << (!axis.actual_position ? axis.actual_position.error()
                                                  : axis.actual_velocity.error())
                             .message()
                        << ")\n";
            }
            return 0;
        }

        [[ = "Enable all drivers and wait 200 ms for settling"_fs,
           = Name{ "motor enable"_fs } ]] static auto enable(Controller& controller,
                                                             const Arguments&,
                                                             std::ostream& out) -> CallbackResult
        {
            controller.enable();
            out << "All drivers enabled and settled.\n";
            return 0;
        }

        [[
            = "Abort all motions, disable holding torque and invalidate references"_fs,
            = Name{ "motor disable"_fs }
        ]] static auto disable(Controller& controller, const Arguments&, std::ostream& out) -> CallbackResult
        {
            controller.disable();
            out << "All drivers disabled; references invalidated.\n";
            return 0;
        }

        [[
            = "Immediate abort of active/queued commands; no deceleration ramp"_fs,
            = Name{ "motor stop"_fs },
            = Arg{ .name = "motor"_fs, .description = "m1, m2, m3 or all (default)"_fs, .optional = true }
        ]] static auto stop(Controller& controller, const Arguments& args, std::ostream& out)
          -> CallbackResult
        {
            controller.stop(selection(args));
            out << "Motion stopped; driver enable unchanged.\n";
            return 0;
        }

        [[
            = "Recover a stopped/faulted timebase; drivers must be disabled"_fs,
            = Name{ "motor reset"_fs }
        ]] static auto reset(Controller& controller, const Arguments&, std::ostream& out) -> CallbackResult
        {
            controller.reset();
            out << "Timebase restarted; drivers remain disabled.\n";
            return 0;
        }

        [[
            = "Relative move; negative values move away from reference"_fs,
            = Name{ "motor move"_fs },
            = Arg{ .name = "motor"_fs, .description = "m1, m2 or m3 (also 1, 2, 3)"_fs },
            = Arg{ .name = "degrees"_fs, .description = "Signed motor-shaft displacement"_fs },
            = Flag{ .name = "accel"_fs,
                    .description = "Acceleration magnitude; 0 uses the axis default"_fs,
                    .value_name = "deg-s2"_fs },
            = Flag{ .name = "decel"_fs,
                    .description = "Deceleration magnitude; 0 uses the axis default"_fs,
                    .value_name = "deg-s2"_fs },
            = Flag{ .name = "jerk"_fs,
                    .description = "Jerk magnitude; 0 uses the axis default"_fs,
                    .value_name = "deg-s3"_fs },
            = Flag{ .name = "speed"_fs,
                    .description = "Required positive motor-shaft speed"_fs,
                    .value_name = "rpm"_fs },
            = Flag{ .name = "timeout"_fs,
                    .description = "Execution timeout; 0 means unlimited"_fs,
                    .value_name = "seconds"_fs },
            =
              Flag{
                .name = "buffer"_fs,
                .description =
                  "aborting (default), buffered, blending-low, blending-previous, blending-next, blending-high; one queued successor"_fs,
                .value_name = "mode"_fs }
        ]] static auto move(Controller& controller, const Arguments& args, std::ostream& out)
          -> CallbackResult
        {
            return performMove(controller, args, out, false);
        }

        [[
            = "Absolute move; successful referencing required"_fs,
            = Name{ "motor moveto"_fs },
            = Arg{ .name = "motor"_fs, .description = "m1, m2 or m3 (also 1, 2, 3)"_fs },
            = Arg{ .name = "degrees"_fs, .description = "Absolute motor-shaft target"_fs },
            = Flag{ .name = "accel"_fs,
                    .description = "Acceleration magnitude; 0 uses the axis default"_fs,
                    .value_name = "deg-s2"_fs },
            = Flag{ .name = "decel"_fs,
                    .description = "Deceleration magnitude; 0 uses the axis default"_fs,
                    .value_name = "deg-s2"_fs },
            = Flag{ .name = "jerk"_fs,
                    .description = "Jerk magnitude; 0 uses the axis default"_fs,
                    .value_name = "deg-s3"_fs },
            = Flag{ .name = "speed"_fs,
                    .description = "Required positive motor-shaft speed"_fs,
                    .value_name = "rpm"_fs },
            = Flag{ .name = "timeout"_fs,
                    .description = "Execution timeout; 0 means unlimited"_fs,
                    .value_name = "seconds"_fs },
            =
              Flag{
                .name = "buffer"_fs,
                .description =
                  "aborting (default), buffered, blending-low, blending-previous, blending-next, blending-high; one queued successor"_fs,
                .value_name = "mode"_fs }
        ]] static auto moveto(Controller& controller, const Arguments& args, std::ostream& out)
          -> CallbackResult
        {
            return performMove(controller, args, out, true);
        }

        [[
            = "Abort this motor's motion and reference using seek/release/slow latch"_fs,
            = Name{ "motor home"_fs },
            = Arg{ .name = "motor"_fs, .description = "m1, m2 or m3 (also 1, 2, 3)"_fs },
            = Flag{ .name = "seek"_fs,
                    .description = "Initial seek speed (default 5)"_fs,
                    .value_name = "rpm"_fs },
            = Flag{ .name = "latch"_fs,
                    .description = "Slow re-latch speed (default 0.5)"_fs,
                    .value_name = "rpm"_fs },
            = Flag{ .name = "timeout"_fs,
                    .description = "Whole reference timeout (default 30, must be > 0)"_fs,
                    .value_name = "seconds"_fs }
        ]] static auto home(Controller& controller, const Arguments& args, std::ostream& out)
          -> CallbackResult
        {
            const auto id{ motorId(args.require("motor")) };
            const auto seek{ numericOption(args, "seek", 5, false) };
            const auto latch{ numericOption(args, "latch", 0.5, false) };
            const auto timeout{ numericOption(args, "timeout", 30, false) };
            if (latch >= seek)
                return callback_failure("latch speed must be slower than seek speed");
            return submitted(controller.reference(id, seek * 1_rpm, latch * 1_rpm, timeout * 1_s), out);
        }

        [[
            = "Change speed of an active move; keeps endpoint and dynamics limits"_fs,
            = Name{ "motor speed"_fs },
            = Arg{ .name = "motor"_fs, .description = "m1, m2 or m3 (also 1, 2, 3)"_fs },
            = Arg{ .name = "rpm"_fs, .description = "Positive new speed magnitude"_fs }
        ]] static auto speed(Controller& controller, const Arguments& args, std::ostream& out)
          -> CallbackResult
        {
            const auto id{ motorId(args.require("motor")) };
            const auto speed{ magnitude(number(args.require("rpm"), "speed"), "speed", false) };
            const auto pulse{ controller.setVelocity(id, speed * 1_rpm) };
            out << std::format("Speed update scheduled from pulse {} (1-based).\n", pulse);
            return 0;
        }

        [[
            = "Show/update idle-axis dynamics in RAM; configured jerk 0 selects a trapezoid"_fs,
            = Name{ "motor defaults"_fs },
            = Arg{ .name = "motor"_fs, .description = "m1, m2 or m3 (also 1, 2, 3)"_fs },
            = Flag{ .name = "accel"_fs,
                    .description = "Positive default acceleration"_fs,
                    .value_name = "deg-s2"_fs },
            = Flag{ .name = "decel"_fs,
                    .description = "Positive default deceleration"_fs,
                    .value_name = "deg-s2"_fs },
            = Flag{ .name = "jerk"_fs,
                    .description = "Nonnegative default jerk; 0 disables jerk limiting"_fs,
                    .value_name = "deg-s3"_fs }
        ]] static auto defaults(Controller& controller, const Arguments& args, std::ostream& out)
          -> CallbackResult
        {
            const auto id{ motorId(args.require("motor")) };
            auto defaults{ controller.defaults(id) };
            defaults.acceleration =
              numericOption(
                args, "accel", defaults.acceleration.get<AngularAccelerationUnits::deg_s2>(), false) *
              1_deg_s2;
            defaults.deceleration =
              numericOption(
                args, "decel", defaults.deceleration.get<AngularAccelerationUnits::deg_s2>(), false) *
              1_deg_s2;
            defaults.jerk =
              numericOption(args, "jerk", defaults.jerk.get<AngularJerkUnits::deg_s3>()) * 1_deg_s3;
            if (args.hasFlag("accel") || args.hasFlag("decel") || args.hasFlag("jerk"))
                controller.setDefaults(id, defaults);
            out << std::format("m{}: accel={:.6g} deg/s^2  decel={:.6g} deg/s^2  jerk={:.6g} deg/s^3\n",
                               static_cast<unsigned>(id) + 1U,
                               defaults.acceleration.get<AngularAccelerationUnits::deg_s2>(),
                               defaults.deceleration.get<AngularAccelerationUnits::deg_s2>(),
                               defaults.jerk.get<AngularJerkUnits::deg_s3>());
            return 0;
        }

        [[
            = "Most recent 32 motion results; outstanding includes planning/executing/queued"_fs,
            = Name{ "motor jobs"_fs },
            = Arg{ .name = "id"_fs, .description = "Optional motion ID"_fs, .optional = true }
        ]] static auto jobs(Controller& controller, const Arguments& args, std::ostream& out)
          -> CallbackResult
        {
            return control_detail::jobs(args, out, controller, "motor");
        }
    }

    void setup(Parser& parser, std::shared_ptr<control::MotionController> controller)
    {
        if (!controller)
            throw std::invalid_argument("motor commands require a controller");
        register_commands(parser, [controller](auto command) {
            return [controller, command](const Arguments& args, std::ostream& out) {
                return command(*controller, args, out);
            };
        });
    }
}
