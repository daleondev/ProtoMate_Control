#include "motion.hpp"
#include "motion_common.hpp"

#include "Parser.hpp"
#include "control/MotionController.hpp"

#include <charconv>
#include <cmath>
#include <format>
#include <ostream>
#include <stdexcept>

namespace cli::motion
{
    namespace detail
    {
        using Controller = control::MotionController;
        using MotorId = Controller::MotorId;
        using namespace pnm::units;

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

        MotorId motor(std::string_view text)
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
            return !text || *text == "all" ? std::nullopt : std::optional{ motor(*text) };
        }

        double number(std::string_view text, std::string_view name)
        {
            double value{};
            // from_chars deliberately ignores no whitespace and no trailing junk.
            if (text.starts_with('+'))
                text.remove_prefix(1);
            const auto result{ std::from_chars(text.data(), text.data() + text.size(), value) };
            if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
                !std::isfinite(value))
                throw std::invalid_argument(std::string(name) + " must be a finite number");
            return value;
        }

        double magnitude(double value, std::string_view name, bool zero)
        {
            if (value < 0.0 || (!zero && value == 0.0))
                throw std::invalid_argument(std::string(name) + (zero ? " must be >= 0" : " must be > 0"));
            return value;
        }

        std::optional<std::string_view> option(const Arguments& args, std::string_view name)
        {
            if (args.flagCount(name) > 1U)
                throw std::invalid_argument("duplicate flag --" + std::string(name));
            return args.flagValue(name);
        }

        double numericOption(const Arguments& args, std::string_view name, double fallback, bool zero)
        {
            const auto value{ option(args, name) };
            return value ? magnitude(number(*value, name), name, zero) : fallback;
        }

        StepperMotor::BufferMode bufferMode(const Arguments& args, std::string_view family)
        {
            using enum StepperMotor::BufferMode;
            const auto value{ option(args, "buffer").value_or("aborting") };
            if (value == "aborting")
                return Aborting;
            if (value == "buffered")
                return Buffered;
            if (value == "blending-low")
                return BlendingLow;
            if (value == "blending-previous")
                return BlendingPrevious;
            if (value == "blending-next")
                return BlendingNext;
            if (value == "blending-high")
                return BlendingHigh;
            throw std::invalid_argument("unknown buffer mode; see 'help " + std::string(family) + " move'");
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
            return "unknown";
        }

        std::string_view stateName(hal::step::State state)
        {
            using enum hal::step::State;
            switch (state) {
                case Idle:
                    return "idle";
                case Ready:
                    return "ready";
                case Running:
                    return "running";
                case Completed:
                    return "completed";
                case Stopped:
                    return "stopped";
                case Underrun:
                    return "underrun";
                case DmaError:
                    return "DMA-error";
            }
            return "unknown";
        }

        std::string_view axisName(MotorId motor)
        {
            switch (motor) {
                case MotorId::Motor1:
                    return "shoulder";
                case MotorId::Motor2:
                    return "elbow";
                case MotorId::Motor3:
                    return "z";
            }
            throw std::invalid_argument("invalid motor/axis");
        }

        void printMotion(const Controller::Motion& value, std::ostream& out, std::string_view family)
        {
            const auto label{ family == "axis"
                                ? std::string(axisName(value.motor))
                                : std::format("m{}", static_cast<unsigned>(value.motor) + 1U) };
            out << std::format("#{} {} {}\n", value.id, label, resultName(value.result));
        }

        int submitted(const Controller::Motion& value, std::ostream& out, std::string_view family)
        {
            printMotion(value, out, family);
            if (!value.result)
                out << std::format("Check completion with: {} jobs {}\n", family, value.id);
            else if (*value.result != StepperMotor::Result::Completed)
                return 1;
            return 0;
        }

        CallbackResult jobs(const Arguments& args,
                            std::ostream& out,
                            Controller& controller,
                            std::string_view family)
        {
            std::optional<Controller::MotionId> selected;
            if (const auto text{ args.get("id") }) {
                Controller::MotionId id{};
                const auto result{ std::from_chars(text->data(), text->data() + text->size(), id) };
                if (result.ec != std::errc{} || result.ptr != text->data() + text->size() || id == 0)
                    return callback_failure("id must be a positive integer");
                selected = id;
            }
            bool found{};
            for (const auto& motion : controller.motions()) {
                if (selected && *selected != motion.id)
                    continue;
                found = true;
                printMotion(motion, out, family);
                if (selected && motion.result && *motion.result != StepperMotor::Result::Completed)
                    return 1;
            }
            if (!found && selected)
                return callback_failure("unknown or expired motion ID");
            if (!found)
                out << "No motions submitted.\n";
            return 0;
        }

        FlagSpec flag(std::string name, std::string description, std::string unit)
        {
            return { std::move(name), std::nullopt, std::move(description), std::move(unit) };
        }

        std::vector<FlagSpec> dynamicsFlags()
        {
            return { flag("accel", "Acceleration magnitude; 0 uses the axis default", "deg-s2"),
                     flag("decel", "Deceleration magnitude; 0 uses the axis default", "deg-s2"),
                     flag("jerk", "Jerk magnitude; 0 uses the axis default", "deg-s3") };
        }
    }

    void setup(Parser& parser, std::shared_ptr<control::MotionController> controller)
    {
        using namespace detail;
        if (!controller)
            throw std::invalid_argument("motion commands require a controller");
        const auto add = [&](Command command) {
            if (const auto result{ parser.registerCommand(std::move(command)) }; !result)
                throw std::runtime_error(result.error());
        };
        const std::vector<ArgumentSpec> one_motor{ { "motor", "m1, m2 or m3 (also 1, 2, 3)" } };
        const std::vector<ArgumentSpec> optional_motor{ { "motor", "m1, m2, m3 or all (default)", true } };
        add({ "motor",
              "Motor commands and examples; help motor <command> for details",
              {},
              {},
              [](const Arguments&, std::ostream& out) {
            out << guide;
            return 0;
        } });
        add({ "motor status",
              "Live commanded/encoder values; all units refer to motor shafts",
              optional_motor,
              {},
              [controller](const Arguments& args, std::ostream& out) {
            const auto selected{ selection(args) };
            const auto status{ controller->status() };
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
        } });
        add({ "motor enable",
              "Enable all drivers and wait 200 ms for settling",
              {},
              {},
              [controller](const Arguments&, std::ostream& out) {
            controller->enable();
            out << "All drivers enabled and settled.\n";
            return 0;
        } });
        add({ "motor disable",
              "Abort all motions, disable holding torque and invalidate references",
              {},
              {},
              [controller](const Arguments&, std::ostream& out) {
            controller->disable();
            out << "All drivers disabled; references invalidated.\n";
            return 0;
        } });
        add({ "motor stop",
              "Immediate abort of active/queued commands; no deceleration ramp",
              optional_motor,
              {},
              [controller](const Arguments& args, std::ostream& out) {
            controller->stop(selection(args));
            out << "Motion stopped; driver enable unchanged.\n";
            return 0;
        } });
        add({ "motor reset",
              "Recover a stopped/faulted timebase; drivers must be disabled",
              {},
              {},
              [controller](const Arguments&, std::ostream& out) {
            controller->reset();
            out << "Timebase restarted; drivers remain disabled.\n";
            return 0;
        } });
        for (const bool absolute : { false, true }) {
            auto flags{ dynamicsFlags() };
            flags.push_back(flag("speed", "Required positive motor-shaft speed", "rpm"));
            flags.push_back(flag("timeout", "Execution timeout; 0 means unlimited", "seconds"));
            flags.push_back(flag("buffer",
                                 "aborting (default), buffered, blending-low, blending-previous, "
                                 "blending-next, blending-high; one queued successor",
                                 "mode"));
            auto arguments{ one_motor };
            arguments.push_back(
              { "degrees", absolute ? "Absolute motor-shaft target" : "Signed motor-shaft displacement" });
            add({ absolute ? "motor moveto" : "motor move",
                  absolute ? "Absolute move; successful referencing required"
                           : "Relative move; negative values move away from reference",
                  std::move(arguments),
                  std::move(flags),
                  [controller, absolute](const Arguments& args, std::ostream& out) -> CallbackResult {
                const auto id{ motor(args.require("motor")) };
                const auto speed{ option(args, "speed") };
                if (!speed)
                    return callback_failure("--speed <rpm> is required");
                const Controller::Move request{ .position =
                                                  number(args.require("degrees"), "degrees") * 1_deg,
                                                .velocity =
                                                  magnitude(number(*speed, "speed"), "speed", false) * 1_rpm,
                                                .acceleration = numericOption(args, "accel", 0) * 1_deg_s2,
                                                .deceleration = numericOption(args, "decel", 0) * 1_deg_s2,
                                                .jerk = numericOption(args, "jerk", 0) * 1_deg_s3,
                                                .buffer = bufferMode(args),
                                                .timeout = numericOption(args, "timeout", 0) * 1_s,
                                                .absolute = absolute };
                return submitted(controller->move(id, request), out);
            } });
        }
        add({ "motor home",
              "Abort this motor's motion and reference using seek/release/slow latch",
              one_motor,
              { flag("seek", "Initial seek speed (default 5)", "rpm"),
                flag("latch", "Slow re-latch speed (default 0.5)", "rpm"),
                flag("timeout", "Whole reference timeout (default 30, must be > 0)", "seconds") },
              [controller](const Arguments& args, std::ostream& out) -> CallbackResult {
            const auto id{ motor(args.require("motor")) };
            const auto seek{ numericOption(args, "seek", 5, false) };
            const auto latch{ numericOption(args, "latch", 0.5, false) };
            const auto timeout{ numericOption(args, "timeout", 30, false) };
            if (latch >= seek)
                return callback_failure("latch speed must be slower than seek speed");
            return submitted(controller->reference(id, seek * 1_rpm, latch * 1_rpm, timeout * 1_s), out);
        } });
        add({ "motor speed",
              "Change speed of an active move; keeps endpoint and dynamics limits",
              { one_motor.front(), { "rpm", "Positive new speed magnitude" } },
              {},
              [controller](const Arguments& args, std::ostream& out) {
            const auto id{ motor(args.require("motor")) };
            const auto speed{ magnitude(number(args.require("rpm"), "speed"), "speed", false) };
            const auto pulse{ controller->setVelocity(id, speed * 1_rpm) };
            out << std::format("Speed update scheduled from pulse {} (1-based).\n", pulse);
            return 0;
        } });
        add({ "motor defaults",
              "Show/update idle-axis dynamics in RAM; configured jerk 0 selects a trapezoid",
              one_motor,
              { flag("accel", "Positive default acceleration", "deg-s2"),
                flag("decel", "Positive default deceleration", "deg-s2"),
                flag("jerk", "Nonnegative default jerk; 0 disables jerk limiting", "deg-s3") },
              [controller](const Arguments& args, std::ostream& out) {
            const auto id{ motor(args.require("motor")) };
            auto defaults{ controller->defaults(id) };
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
                controller->setDefaults(id, defaults);
            out << std::format("m{}: accel={:.6g} deg/s^2  decel={:.6g} deg/s^2  jerk={:.6g} deg/s^3\n",
                               static_cast<unsigned>(id) + 1U,
                               defaults.acceleration.get<AngularAccelerationUnits::deg_s2>(),
                               defaults.deceleration.get<AngularAccelerationUnits::deg_s2>(),
                               defaults.jerk.get<AngularJerkUnits::deg_s3>());
            return 0;
        } });
        add({ "motor jobs",
              "Most recent 32 motion results; outstanding includes planning/executing/queued",
              { { "id", "Optional motion ID", true } },
              {},
              [controller](const Arguments& args, std::ostream& out) -> CallbackResult {
            return jobs(args, out, *controller, "motor");
        } });
    }
}
