#include "motor.hpp"
#include "control_common.hpp"

#include <format>
#include <cmath>
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
            "  motor driver status [m2|m3]         UART configuration and diagnostics\n"
            "  motor driver configure <motor>     Current/mode settings; disabled only\n"
            "  motor driver init                  Recover both UART drivers; disabled only\n"
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
            = "Live commanded and feedback values; all units refer to motor shafts"_fs,
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
                const auto source{ axis.feedback_source == StepperMotor::FeedbackSource::DriverIndex
                                     ? "driver INDEX (pseudo)" : "encoder" };
                if (axis.actual_position && axis.actual_velocity)
                    out << std::format("    {}: position={:.4f} deg  velocity={:.3f} rpm\n", source,
                                       axis.actual_position->get<AngleUnits::deg>(),
                                       axis.actual_velocity->get<AngularVelocityUnits::rpm>());
                else if (!axis.actual_position && axis.actual_position.error() == std::errc::no_such_device)
                    out << "    " << source << ": not fitted\n";
                else if (!axis.actual_position && axis.actual_position.error() == std::errc::no_message_available)
                    out << "    " << source << ": waiting for INDEX\n";
                else
                    out << "    " << source << ": unavailable ("
                        << (!axis.actual_position ? axis.actual_position.error()
                                                  : axis.actual_velocity.error())
                             .message()
                        << ")\n";
                if (axis.feedback_source == StepperMotor::FeedbackSource::DriverIndex)
                    out << std::format("    resolution={:.4f} deg; electrical phase, not shaft sensing\n",
                                       axis.feedback_resolution.get<AngleUnits::deg>());
            }
            return 0;
        }

        [[ = "Read cached M2/M3 driver diagnostics (100 ms polling); M1 uses DIP switches"_fs,
           = Name{ "motor driver status"_fs },
           = Arg{ .name = "motor"_fs, .description = "m2, m3 or all (default)"_fs, .optional = true } ]]
        static auto driverStatus(Controller& controller, const Arguments& args, std::ostream& out) -> CallbackResult
        {
            using Driver = hal::device::Tmc2209;
            auto selected = selection(args);
            if (selected == MotorId::Motor1) {
                out << "m1: DM542T; configuration via DIP switches, no UART.\n";
                return 0;
            }
            for (const auto& d : controller.driverStatus()) {
                if (selected && *selected != d.motor) continue;
                auto run = Driver::currentScale(d.configuration.run_milliamps);
                auto hold = Driver::currentScale(d.configuration.hold_milliamps);
                out << std::format("m{}: UART address={} ready={} fault-latched={} mode={} microsteps={} interpolation={}\n"
                                   "    run={} mA RMS (nominal quantized {}), hold={} mA RMS (nominal quantized {})\n",
                    static_cast<unsigned>(d.motor) + 1, d.address, d.ready, d.fault_latched,
                    d.configuration.mode == Driver::Mode::SpreadCycle ? "spreadcycle" : "stealthchop",
                    d.configuration.microsteps, d.configuration.interpolate,
                    d.configuration.run_milliamps, run ? Driver::currentMilliamps(*run) : 0,
                    d.configuration.hold_milliamps, hold ? Driver::currentMilliamps(*hold) : 0);
                if (d.diagnostics) {
                    const auto& s = *d.diagnostics;
                    out << std::format("    reset={} fault={} overtemperature-warning={} open-load={} standstill={}\n"
                                       "    GSTAT=0x{:08x} DRV_STATUS=0x{:08x} IOIN=0x{:08x} SG_RESULT={}\n",
                        s.reset(), s.fault(), s.warning(), s.openLoad(), s.standstill(),
                        s.global, s.driver, s.input, s.load);
                }
                if (!d.error.empty()) out << "    error: " << d.error << '\n';
            }
            out << "Temperature flags are thresholds, not measured degrees. Open-load can be false at standstill.\n"
                   "SG_RESULT needs StealthChop at suitable speed; it is not encoder feedback.\n";
            return 0;
        }

        [[ = "Initialize and verify both UART drivers; disabled only, invalidates references"_fs,
           = Name{ "motor driver init"_fs } ]]
        static auto driverInit(Controller& controller, const Arguments&, std::ostream& out) -> CallbackResult
        {
            controller.initializeDrivers();
            out << "Both TMC2209 drivers verified; drivers remain disabled.\n";
            return 0;
        }

        [[ = "Configure M2/M3 in RAM while disabled; microsteps follow axis configuration"_fs,
           = Name{ "motor driver configure"_fs },
           = Arg{ .name = "motor"_fs, .description = "m2 or m3"_fs },
           = Flag{ .name = "run"_fs, .description = "RMS current: M2 100..700, M3 100..590; rounded down"_fs, .value_name = "mA"_fs },
           = Flag{ .name = "hold"_fs, .description = "RMS holding current <= run; Z must equal run"_fs, .value_name = "mA"_fs },
           = Flag{ .name = "mode"_fs, .description = "spreadcycle or stealthchop (>=512 mA)"_fs, .value_name = "mode"_fs },
           = Flag{ .name = "interpolate"_fs, .description = "Internal 256-microstep interpolation: on or off"_fs, .value_name = "state"_fs } ]]
        static auto driverConfigure(Controller& controller, const Arguments& args, std::ostream& out) -> CallbackResult
        {
            const auto id = motorId(args.require("motor"));
            if (id == MotorId::Motor1) return callback_failure("m1 uses the DM542T DIP switches");
            auto config = controller.driverStatus()[static_cast<unsigned>(id) - 1].configuration;
            auto current = [&](std::string_view name, std::uint16_t fallback) {
                const auto value = numericOption(args, name, fallback, false);
                if (value > 1800 || std::floor(value) != value)
                    throw std::invalid_argument("current must be an integer in mA RMS");
                return static_cast<std::uint16_t>(value);
            };
            config.run_milliamps = current("run", config.run_milliamps);
            config.hold_milliamps = current("hold", id == MotorId::Motor3 ? config.run_milliamps : config.hold_milliamps);
            if (auto mode = option(args, "mode")) {
                if (*mode == "spreadcycle") config.mode = hal::device::Tmc2209::Mode::SpreadCycle;
                else if (*mode == "stealthchop") config.mode = hal::device::Tmc2209::Mode::StealthChop;
                else return callback_failure("mode must be spreadcycle or stealthchop");
            }
            if (auto interpolate = option(args, "interpolate")) {
                if (*interpolate != "on" && *interpolate != "off") return callback_failure("interpolate must be on or off");
                config.interpolate = *interpolate == "on";
            }
            controller.configureDriver(id, config);
            out << "Driver settings applied and verified in RAM; drivers remain disabled.\n";
            return 0;
        }

        [[ = "Verify UART drivers, enable all drivers and wait 200 ms for settling"_fs,
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
