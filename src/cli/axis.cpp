#include "axis.hpp"
#include "motion_common.hpp"

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
        using namespace motion::detail;

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

        Controller::AxisSpeed speed(MotorId id, double value)
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
    }

    void setup(Parser& parser, std::shared_ptr<Controller> controller)
    {
        if (!controller)
            throw std::invalid_argument("axis commands require a controller");
        const auto add = [&](Command command) {
            if (const auto result{ parser.registerCommand(std::move(command)) }; !result)
                throw std::runtime_error(result.error());
        };
        const std::vector<ArgumentSpec> one_axis{ { "axis",
                                                    "shoulder, elbow or z (also a1/a2/a3 or 1/2/3)" } };
        const std::vector<ArgumentSpec> optional_axis{
            { "axis", "shoulder, elbow, z or all (default)", true }
        };
        add({ "axis",
              "Joint-axis commands in degrees/deg/s or mm/mm/s",
              {},
              {},
              [](const Arguments&, std::ostream& out) {
            out << guide;
            return 0;
        } });
        add({ "axis status",
              "Joint coordinates and signed velocities; unreferenced positions lack a physical datum",
              optional_axis,
              {},
              [controller](const Arguments& args, std::ostream& out) {
            const auto selected{ selection(args) };
            const auto status{ controller->status() };
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
                    if (axis.actual_position && axis.actual_velocity)
                        out << std::format("    encoder: position={:.4f} {}  velocity={:.3f} {}\n",
                                           conversion.toAxisPosition(*axis.actual_position) / position_unit,
                                           linear ? "mm" : "deg",
                                           conversion.toAxisVelocity(*axis.actual_velocity) / speed_unit,
                                           linear ? "mm/s" : "deg/s");
                    else if (!axis.actual_position &&
                             axis.actual_position.error() == std::errc::no_such_device)
                        out << "    encoder: not fitted\n";
                    else
                        out << "    encoder: unavailable ("
                            << (!axis.actual_position ? axis.actual_position.error()
                                                      : axis.actual_velocity.error())
                                 .message()
                            << ")\n";
                }, *axis.conversion);
            }
            return 0;
        } });
        for (const bool absolute : { false, true }) {
            auto arguments{ one_axis };
            arguments.push_back({ "value",
                                  absolute ? "Joint target (degrees, or mm for Z)"
                                           : "Signed joint displacement (degrees, or mm for Z)" });
            add({ absolute ? "axis moveto" : "axis move",
                  absolute ? "Absolute joint move; reference required"
                           : "Relative joint move; sign follows configured axis direction",
                  std::move(arguments),
                  { flag("speed", "Required positive joint speed: deg/s or mm/s for Z", "value"),
                    flag("accel", "Acceleration magnitude: deg/s^2 or mm/s^2; 0 uses default", "value"),
                    flag("decel", "Deceleration magnitude: deg/s^2 or mm/s^2; 0 uses default", "value"),
                    flag("jerk", "Jerk magnitude: deg/s^3 or mm/s^3; 0 uses default", "value"),
                    flag("timeout", "Execution timeout; 0 means unlimited", "seconds"),
                    flag("buffer",
                         "aborting (default), buffered, blending-low, blending-previous, blending-next, "
                         "blending-high",
                         "mode") },
                  [controller, absolute](const Arguments& args, std::ostream& out) {
                const auto id{ axisId(args.require("axis")) };
                const auto request{ id == MotorId::Motor3 ? moveRequest<true>(args, absolute)
                                                          : moveRequest<false>(args, absolute) };
                return submitted(controller->moveAxis(id, request), out, "axis");
            } });
        }
        add({ "axis home",
              "Reference one axis; omitted speeds retain 5/0.5 motor rpm. Direction remains motor Forward",
              one_axis,
              { flag("seek", "Seek speed: deg/s or mm/s for Z (>0)", "value"),
                flag("latch", "Latch speed: deg/s or mm/s for Z; slower than seek (>0)", "value"),
                flag("timeout", "Overall timeout (>0, default 30)", "seconds") },
              [controller](const Arguments& args, std::ostream& out) {
            const auto id{ axisId(args.require("axis")) };
            const auto optional_speed = [&](std::string_view name) -> std::optional<Controller::AxisSpeed> {
                if (const auto value{ option(args, name) })
                    return speed(id, magnitude(number(*value, name), name, false));
                return {};
            };
            const auto seek{ optional_speed("seek") }, latch{ optional_speed("latch") };
            const auto timeout{ numericOption(args, "timeout", 30, false) * 1_s };
            return submitted(controller->referenceAxis(id, seek, latch, timeout), out, "axis");
        } });
        add({ "axis speed",
              "Replan remaining motion at positive joint speed (endpoint unchanged)",
              { one_axis.front(), { "value", "deg/s for shoulder/elbow, mm/s for Z" } },
              {},
              [controller](const Arguments& args, std::ostream& out) {
            const auto id{ axisId(args.require("axis")) };
            const auto value{ magnitude(number(args.require("value"), "speed"), "speed", false) };
            out << std::format("Speed update scheduled from pulse {} (1-based).\n",
                               controller->setAxisVelocity(id, speed(id, value)));
            return 0;
        } });
        add({ "axis defaults",
              "Show/update idle-axis profile limits in joint units (same settings as motor defaults)",
              one_axis,
              { flag("accel", "Positive acceleration: deg/s^2 or mm/s^2", "value"),
                flag("decel", "Positive deceleration: deg/s^2 or mm/s^2", "value"),
                flag("jerk", "Nonnegative jerk: deg/s^3 or mm/s^3; 0 selects trapezoid", "value") },
              [controller](const Arguments& args, std::ostream& out) {
            const auto id{ axisId(args.require("axis")) };
            auto defaults{ controller->axisDefaults(id) };
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
                controller->setAxisDefaults(id, defaults);
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
        } });
        add({ "axis enable",
              "Enable ALL drivers and wait 200 ms",
              {},
              {},
              [controller](const Arguments&, std::ostream& out) {
            controller->enable();
            out << "All drivers enabled and settled.\n";
            return 0;
        } });
        add({ "axis disable",
              "Stop ALL motors, disable holding torque and invalidate references",
              {},
              {},
              [controller](const Arguments&, std::ostream& out) {
            controller->disable();
            out << "All drivers disabled; references invalidated.\n";
            return 0;
        } });
        add({ "axis stop",
              "Immediate abort of selected axis or all; retains holding torque",
              optional_axis,
              {},
              [controller](const Arguments& args, std::ostream& out) {
            controller->stop(selection(args));
            out << "Motion stopped; driver enable unchanged.\n";
            return 0;
        } });
        add({ "axis reset",
              "Restart shared timebase while drivers are disabled",
              {},
              {},
              [controller](const Arguments&, std::ostream& out) {
            controller->reset();
            out << "Timebase restarted; drivers remain disabled.\n";
            return 0;
        } });
        add({ "axis jobs",
              "Same 32 motion results as motor jobs, labelled by joint axis",
              { { "id", "Optional motion ID", true } },
              {},
              [controller](const Arguments& args, std::ostream& out) {
            return jobs(args, out, *controller, "axis");
        } });
    }
}
