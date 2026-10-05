#include "axis.hpp"
#include "control_common.hpp"

#include <format>
#include <ostream>
#include <type_traits>

namespace cli::axis
{
    namespace
    {
        using Controller = control::MotionController;
        using MotorId = Controller::MotorId;
        using namespace pnm::units;
        using namespace control_detail;

        constexpr auto degrees_per_second{ 1_deg / 1_s };
        constexpr std::string_view guide{
            "Joint-axis control: shoulder / elbow / z (also a1 / a2 / a3 or 1 / 2 / 3).\n"
            "Shoulder/elbow: degrees, deg/s, deg/s^2, deg/s^3. Z: mm, mm/s, mm/s^2, mm/s^3.\n"
            "  axis status [axis|all]                Joint positions, feedback and reference state\n"
            "  axis enable / axis disable           Shared driver enable\n"
            "  axis home <axis>                     Reference in the motor's fixed seek direction\n"
            "  axis move <axis> <distance> --speed <value>    Relative joint move\n"
            "  axis moveto <axis> <position> --speed <value>  Absolute joint move (reference required)\n"
            "  axis speed <axis> <value>            Change speed of an active motion\n"
            "  axis defaults <axis>                 Show/change joint dynamics\n"
            "  axis stop [axis|all]                 Immediate abort; retains holding torque\n"
            "  axis jobs [id]                       Same motion IDs/results as motor jobs\n"
            "  axis reset                           Restart timebase while disabled\n"
            "Use 'help axis move', 'help axis home', etc. for options.\n"
            "Joint mechanics come from the application configuration.\n"
        };

        MotorId axisId(std::string_view value)
        {
            if (value == "shoulder" || value == "a1" || value == "1")
                return MotorId::Motor1;
            if (value == "elbow" || value == "a2" || value == "2")
                return MotorId::Motor2;
            if (value == "z" || value == "a3" || value == "3")
                return MotorId::Motor3;
            throw std::invalid_argument("axis must be shoulder, elbow or z (also a1/a2/a3 or 1/2/3)");
        }

        std::optional<MotorId> selection(const Arguments& args)
        {
            const auto value{ args.get("axis") };
            return !value || *value == "all" ? std::nullopt : std::optional{ axisId(*value) };
        }

        Controller::AxisSpeed axisSpeed(MotorId id, double value)
        {
            if (id == MotorId::Motor3)
                return value * 1_mm_s;
            return value * degrees_per_second;
        }

        template<bool Linear>
        Controller::AxisMove moveRequest(const Arguments& args, bool absolute)
        {
            using Request = std::conditional_t<Linear, Controller::LinearMove, Controller::Move>;
            constexpr auto position_unit{ [] {
                if constexpr (Linear)
                    return 1_mm;
                else
                    return 1_deg;
            }() };
            constexpr auto speed_unit{ position_unit / 1_s };
            constexpr auto acceleration_unit{ speed_unit / 1_s };
            constexpr auto jerk_unit{ acceleration_unit / 1_s };
            const auto requested_speed{ option(args, "speed") };
            if (!requested_speed)
                throw std::invalid_argument("--speed is required (deg/s for rotary axes; mm/s for Z)");
            return Request{
                .position = number(args.require("value"), "position/displacement") * position_unit,
                .velocity = magnitude(number(*requested_speed, "speed"), "speed", false) * speed_unit,
                .acceleration = numericOption(args, "accel", 0) * acceleration_unit,
                .deceleration = numericOption(args, "decel", 0) * acceleration_unit,
                .jerk = numericOption(args, "jerk", 0) * jerk_unit,
                .buffer = bufferMode(args, "axis"),
                .timeout = numericOption(args, "timeout", 0) * 1_s,
                .absolute = absolute,
            };
        }
        auto performMove(Controller& controller, const Arguments& args, std::ostream& out, bool absolute)
          -> CallbackResult
        {
            const auto id{ axisId(args.require("axis")) };
            const auto request{ id == MotorId::Motor3 ? moveRequest<true>(args, absolute)
                                                      : moveRequest<false>(args, absolute) };
            return submitted(controller.moveAxis(id, request), out, "axis");
        }
    }

    namespace commands
    {
        using namespace pnm::meta::string::literals;

        [[ = "Joint-axis commands in degrees/deg/s or mm/mm/s"_fs,
           = Name{ "axis"_fs } ]] static auto overview(Controller&, const Arguments&, std::ostream& out)
          -> CallbackResult
        {
            out << guide;
            return 0;
        }

        [[
            = "Joint coordinates and signed velocities; unreferenced positions lack a physical datum"_fs,
            = Name{ "axis status"_fs },
            = Arg{ .name = "axis"_fs,
                   .description = "shoulder, elbow, z or all (default)"_fs,
                   .optional = true }
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
                if (selected && axis.motor != *selected)
                    continue;
                const auto index{ static_cast<std::size_t>(axis.motor) };
                out << std::format("{}: referenced={}  switch={}  outstanding={}  step={}\n",
                                   axisName(axis.motor),
                                   axis.referenced ? "yes" : "no",
                                   axis.reference_switch_active ? "active/open" : "released",
                                   axis.outstanding,
                                   stateName(status.generator.axes[index]));
                if (!axis.conversion) {
                    out << "    mechanics not configured\n";
                    continue;
                }
                std::visit([&]<typename Converter>(const Converter& conversion) {
                    constexpr bool linear{ std::is_same_v<Converter, LinearAxisConversion> };
                    constexpr auto position_unit{ [] {
                        if constexpr (linear)
                            return 1_mm;
                        else
                            return 1_deg;
                    }() };
                    constexpr auto speed_unit{ position_unit / 1_s };
                    out << std::format("    position={:.4f} {}  velocity={:.3f} {}\n",
                                       conversion.toAxisPosition(axis.position) / position_unit,
                                       linear ? "mm" : "deg",
                                       conversion.toAxisVelocity(axis.velocity) / speed_unit,
                                       linear ? "mm/s" : "deg/s");
                    const auto source{ axis.feedback_source == StepperMotor::FeedbackSource::DriverIndex
                                         ? "driver INDEX (pseudo)" : "encoder" };
                    if (axis.actual_position && axis.actual_velocity)
                        out << std::format("    {}: position={:.4f} {}  velocity={:.3f} {}\n", source,
                                           conversion.toAxisPosition(*axis.actual_position) / position_unit,
                                           linear ? "mm" : "deg",
                                           conversion.toAxisVelocity(*axis.actual_velocity) / speed_unit,
                                           linear ? "mm/s" : "deg/s");
                    else if (!axis.actual_position &&
                             axis.actual_position.error() == std::errc::no_such_device)
                        out << "    " << source << ": not fitted\n";
                    else if (!axis.actual_position && axis.actual_position.error() == std::errc::no_message_available)
                        out << "    " << source << ": waiting for INDEX\n";
                    else
                        out << "    " << source << ": unavailable ("
                            << (!axis.actual_position ? axis.actual_position.error()
                                                      : axis.actual_velocity.error())
                                 .message()
                            << ")\n";
                }, *axis.conversion);
            }
            return 0;
        }

        [[
            = "Relative joint move; sign follows configured axis direction"_fs,
            = Name{ "axis move"_fs },
            = Arg{ .name = "axis"_fs, .description = "shoulder, elbow or z (also a1/a2/a3 or 1/2/3)"_fs },
            = Arg{ .name = "value"_fs, .description = "Signed joint displacement (degrees, or mm for Z)"_fs },
            = Flag{ .name = "speed"_fs,
                    .description = "Required positive joint speed: deg/s or mm/s for Z"_fs,
                    .value_name = "value"_fs },
            = Flag{ .name = "accel"_fs,
                    .description = "Acceleration magnitude: deg/s^2 or mm/s^2; 0 uses default"_fs,
                    .value_name = "value"_fs },
            = Flag{ .name = "decel"_fs,
                    .description = "Deceleration magnitude: deg/s^2 or mm/s^2; 0 uses default"_fs,
                    .value_name = "value"_fs },
            = Flag{ .name = "jerk"_fs,
                    .description = "Jerk magnitude: deg/s^3 or mm/s^3; 0 uses default"_fs,
                    .value_name = "value"_fs },
            = Flag{ .name = "timeout"_fs,
                    .description = "Execution timeout; 0 means unlimited"_fs,
                    .value_name = "seconds"_fs },
            =
              Flag{
                .name = "buffer"_fs,
                .description =
                  "aborting (default), buffered, blending-low, blending-previous, blending-next, blending-high"_fs,
                .value_name = "mode"_fs }
        ]] static auto move(Controller& controller, const Arguments& args, std::ostream& out)
          -> CallbackResult
        {
            return performMove(controller, args, out, false);
        }

        [[
            = "Absolute joint move; reference required"_fs,
            = Name{ "axis moveto"_fs },
            = Arg{ .name = "axis"_fs, .description = "shoulder, elbow or z (also a1/a2/a3 or 1/2/3)"_fs },
            = Arg{ .name = "value"_fs, .description = "Joint target (degrees, or mm for Z)"_fs },
            = Flag{ .name = "speed"_fs,
                    .description = "Required positive joint speed: deg/s or mm/s for Z"_fs,
                    .value_name = "value"_fs },
            = Flag{ .name = "accel"_fs,
                    .description = "Acceleration magnitude: deg/s^2 or mm/s^2; 0 uses default"_fs,
                    .value_name = "value"_fs },
            = Flag{ .name = "decel"_fs,
                    .description = "Deceleration magnitude: deg/s^2 or mm/s^2; 0 uses default"_fs,
                    .value_name = "value"_fs },
            = Flag{ .name = "jerk"_fs,
                    .description = "Jerk magnitude: deg/s^3 or mm/s^3; 0 uses default"_fs,
                    .value_name = "value"_fs },
            = Flag{ .name = "timeout"_fs,
                    .description = "Execution timeout; 0 means unlimited"_fs,
                    .value_name = "seconds"_fs },
            =
              Flag{
                .name = "buffer"_fs,
                .description =
                  "aborting (default), buffered, blending-low, blending-previous, blending-next, blending-high"_fs,
                .value_name = "mode"_fs }
        ]] static auto moveto(Controller& controller, const Arguments& args, std::ostream& out)
          -> CallbackResult
        {
            return performMove(controller, args, out, true);
        }

        [[
            = "Reference one axis; omitted speeds retain 5/0.5 motor rpm. Direction remains motor Forward"_fs,
            = Name{ "axis home"_fs },
            = Arg{ .name = "axis"_fs, .description = "shoulder, elbow or z (also a1/a2/a3 or 1/2/3)"_fs },
            = Flag{ .name = "seek"_fs,
                    .description = "Seek speed: deg/s or mm/s for Z (>0)"_fs,
                    .value_name = "value"_fs },
            = Flag{ .name = "latch"_fs,
                    .description = "Latch speed: deg/s or mm/s for Z; slower than seek (>0)"_fs,
                    .value_name = "value"_fs },
            = Flag{ .name = "timeout"_fs,
                    .description = "Overall timeout (>0, default 30)"_fs,
                    .value_name = "seconds"_fs }
        ]] static auto home(Controller& controller, const Arguments& args, std::ostream& out)
          -> CallbackResult
        {
            const auto id{ axisId(args.require("axis")) };
            const auto optional_speed = [&](std::string_view name) -> std::optional<Controller::AxisSpeed> {
                if (const auto value{ option(args, name) })
                    return axisSpeed(id, magnitude(number(*value, name), name, false));
                return {};
            };
            const auto seek{ optional_speed("seek") }, latch{ optional_speed("latch") };
            const auto timeout{ numericOption(args, "timeout", 30, false) * 1_s };
            return submitted(controller.referenceAxis(id, seek, latch, timeout), out, "axis");
        }

        [[
            = "Replan remaining motion at positive joint speed (endpoint unchanged)"_fs,
            = Name{ "axis speed"_fs },
            = Arg{ .name = "axis"_fs, .description = "shoulder, elbow or z (also a1/a2/a3 or 1/2/3)"_fs },
            = Arg{ .name = "value"_fs, .description = "deg/s for shoulder/elbow, mm/s for Z"_fs }
        ]] static auto speed(Controller& controller, const Arguments& args, std::ostream& out)
          -> CallbackResult
        {
            const auto id{ axisId(args.require("axis")) };
            const auto value{ magnitude(number(args.require("value"), "speed"), "speed", false) };
            out << std::format("Speed update scheduled from pulse {} (1-based).\n",
                               controller.setAxisVelocity(id, axisSpeed(id, value)));
            return 0;
        }

        [[
            = "Show/update idle-axis profile limits in joint units (same settings as motor defaults)"_fs,
            = Name{ "axis defaults"_fs },
            = Arg{ .name = "axis"_fs, .description = "shoulder, elbow or z (also a1/a2/a3 or 1/2/3)"_fs },
            = Flag{ .name = "accel"_fs,
                    .description = "Positive acceleration: deg/s^2 or mm/s^2"_fs,
                    .value_name = "value"_fs },
            = Flag{ .name = "decel"_fs,
                    .description = "Positive deceleration: deg/s^2 or mm/s^2"_fs,
                    .value_name = "value"_fs },
            = Flag{ .name = "jerk"_fs,
                    .description = "Nonnegative jerk: deg/s^3 or mm/s^3; 0 selects trapezoid"_fs,
                    .value_name = "value"_fs }
        ]] static auto defaults(Controller& controller, const Arguments& args, std::ostream& out)
          -> CallbackResult
        {
            const auto id{ axisId(args.require("axis")) };
            auto defaults{ controller.axisDefaults(id) };
            std::visit([&]<typename Defaults>(Defaults& value) {
                constexpr bool linear{ std::is_same_v<Defaults, Controller::LinearDefaults> };
                constexpr auto acceleration_unit{ [] {
                    if constexpr (linear)
                        return 1_mm_s2;
                    else
                        return 1_deg_s2;
                }() };
                constexpr auto jerk_unit{ [] {
                    if constexpr (linear)
                        return 1_mm_s3;
                    else
                        return 1_deg_s3;
                }() };
                value.acceleration =
                  numericOption(args, "accel", value.acceleration / acceleration_unit, false) *
                  acceleration_unit;
                value.deceleration =
                  numericOption(args, "decel", value.deceleration / acceleration_unit, false) *
                  acceleration_unit;
                value.jerk = numericOption(args, "jerk", value.jerk / jerk_unit) * jerk_unit;
            }, defaults);
            if (args.hasFlag("accel") || args.hasFlag("decel") || args.hasFlag("jerk"))
                controller.setAxisDefaults(id, defaults);
            std::visit([&]<typename Defaults>(const Defaults& value) {
                constexpr bool linear{ std::is_same_v<Defaults, Controller::LinearDefaults> };
                constexpr auto acceleration_unit{ [] {
                    if constexpr (linear)
                        return 1_mm_s2;
                    else
                        return 1_deg_s2;
                }() };
                constexpr auto jerk_unit{ [] {
                    if constexpr (linear)
                        return 1_mm_s3;
                    else
                        return 1_deg_s3;
                }() };
                out << std::format("{}: accel={:.6g} {}  decel={:.6g} {}  jerk={:.6g} {}\n",
                                   axisName(id),
                                   value.acceleration / acceleration_unit,
                                   linear ? "mm/s^2" : "deg/s^2",
                                   value.deceleration / acceleration_unit,
                                   linear ? "mm/s^2" : "deg/s^2",
                                   value.jerk / jerk_unit,
                                   linear ? "mm/s^3" : "deg/s^3");
            }, defaults);
            return 0;
        }

        [[ = "Enable ALL drivers and wait 200 ms"_fs,
           = Name{ "axis enable"_fs } ]] static auto enable(Controller& controller,
                                                            const Arguments&,
                                                            std::ostream& out) -> CallbackResult
        {
            controller.enable();
            out << "All drivers enabled and settled.\n";
            return 0;
        }

        [[
            = "Stop ALL motors, disable holding torque and invalidate references"_fs,
            = Name{ "axis disable"_fs }
        ]] static auto disable(Controller& controller, const Arguments&, std::ostream& out) -> CallbackResult
        {
            controller.disable();
            out << "All drivers disabled; references invalidated.\n";
            return 0;
        }

        [[
            = "Immediate abort of selected axis or all; retains holding torque"_fs,
            = Name{ "axis stop"_fs },
            = Arg{ .name = "axis"_fs,
                   .description = "shoulder, elbow, z or all (default)"_fs,
                   .optional = true }
        ]] static auto stop(Controller& controller, const Arguments& args, std::ostream& out)
          -> CallbackResult
        {
            controller.stop(selection(args));
            out << "Motion stopped; driver enable unchanged.\n";
            return 0;
        }

        [[ = "Restart shared timebase while drivers are disabled"_fs,
           = Name{ "axis reset"_fs } ]] static auto reset(Controller& controller,
                                                          const Arguments&,
                                                          std::ostream& out) -> CallbackResult
        {
            controller.reset();
            out << "Timebase restarted; drivers remain disabled.\n";
            return 0;
        }

        [[
            = "Same 32 motion results as motor jobs, labelled by joint axis"_fs,
            = Name{ "axis jobs"_fs },
            = Arg{ .name = "id"_fs, .description = "Optional motion ID"_fs, .optional = true }
        ]] static auto jobs(Controller& controller, const Arguments& args, std::ostream& out)
          -> CallbackResult
        {
            return control_detail::jobs(args, out, controller, "axis");
        }
    }

    void setup(Parser& parser, std::shared_ptr<control::MotionController> controller)
    {
        if (!controller)
            throw std::invalid_argument("axis commands require a controller");
        register_commands(parser, [controller](auto command) {
            return [controller, command](const Arguments& args, std::ostream& out) {
                return command(*controller, args, out);
            };
        });
    }
}
