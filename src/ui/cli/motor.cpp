#include "motor.hpp"
#include "Parser.hpp"
#include "ui/motion/Client.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <mutex>
#include <ostream>
#include <stdexcept>

namespace ui::cli::motor
{
    namespace
    {
        using namespace ::motion;
        struct Context { ui::motion::Client* client; std::stop_token stop; };

        MotorId motorId(std::string_view value)
        {
            if (value == "m1" || value == "1") { return MotorId::M1; }
            if (value == "m2" || value == "2") { return MotorId::M2; }
            if (value == "m3" || value == "3") { return MotorId::M3; }
            throw std::invalid_argument("motor must be m1, m2 or m3 (also 1, 2, 3)");
        }
        MotorId selection(const Arguments& args)
        {
            const auto name{ args.get("motor") };
            return !name || *name == "all" ? MotorId::All : motorId(*name);
        }
        double number(std::string_view text, std::string_view name)
        {
            double value{};
            const auto parsed{ std::from_chars(text.data(), text.data() + text.size(), value) };
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value)) {
                throw std::invalid_argument(std::string(name) + " must be a finite number");
            }
            return value;
        }
        double magnitude(double value, std::string_view name, bool zero = true)
        {
            if (value < 0 || (!zero && value == 0)) {
                throw std::invalid_argument(std::string(name) + (zero ? " must be >= 0" : " must be > 0"));
            }
            return value;
        }
        std::optional<std::string_view> option(const Arguments& args, std::string_view name)
        {
            if (args.flagCount(name) > 1) { throw std::invalid_argument("--" + std::string(name) + " may occur only once"); }
            return args.flagValue(name);
        }
        double numericOption(const Arguments& args, std::string_view name, double fallback, bool zero = true)
        {
            const auto value{ option(args, name) };
            return value ? magnitude(number(*value, name), name, zero) : fallback;
        }
        MotorBuffer bufferMode(const Arguments& args)
        {
            constexpr std::array names{ "aborting", "buffered", "blending-low", "blending-previous", "blending-next", "blending-high" };
            const auto name{ option(args, "buffer").value_or("aborting") };
            for (std::size_t i{}; i < names.size(); ++i) {
                if (name == names[i]) { return static_cast<MotorBuffer>(i); }
            }
            throw std::invalid_argument("invalid buffer mode; see 'help motor move'");
        }
        const char* jobState(ui::motion::Client::JobState state)
        {
            using enum ui::motion::Client::JobState;
            switch (state) {
                case Submitting: return "submitting";
                case Accepted: return "accepted (running or queued)";
                case Completed: return "completed";
                case Stopped: return "stopped";
                case TimedOut: return "timed out";
                case Rejected: return "rejected";
                case Faulted: return "faulted";
                case Unknown: return "unknown (communication/shutdown; stop unconfirmed)";
            }
            return "unknown";
        }
        CallbackResult submit(const Context& context, MotorGoal goal, std::ostream& out)
        {
            const auto job{ context.client->submit(goal, context.stop) };
            out << "Submitted motor job " << job << "; check 'motor jobs " << job << "'.\n";
            return 0;
        }
        CallbackResult move(const Context& context, const Arguments& args, std::ostream& out, bool absolute)
        {
            const auto speed{ option(args, "speed") };
            if (!speed) { return callback_failure("--speed <rpm> is required"); }
            return submit(context, { .motor = motorId(args.require("motor")),
                .motion = absolute ? MotorMotion::Absolute : MotorMotion::Relative,
                .position = number(args.require("degrees"), "degrees"),
                .velocity = magnitude(number(*speed, "speed"), "speed", false),
                .acceleration = numericOption(args, "accel", 0),
                .deceleration = numericOption(args, "decel", 0),
                .jerk = numericOption(args, "jerk", 0),
                .timeout = numericOption(args, "timeout", 0), .buffer = bufferMode(args) }, out);
        }
        CallbackResult status(const Context& context, const Arguments& args, std::ostream& out, bool drivers)
        {
            const auto selected{ selection(args) };
            const auto response{ context.client->state() };
            out << "Drivers: " << (response.enabled ? "enabled" : "disabled")
                << " | controller: " << (response.ready ? "ready" : "not ready") << '\n';
            for (std::size_t i{}; i < response.motors.size(); ++i) {
                if (selected != MotorId::All && static_cast<std::size_t>(selected) != i) { continue; }
                const auto& state{ response.motors[i] };
                if (drivers) {
                    out << std::format("m{} {}: ready={} alarm={} latched={} warning={} fault={}\n",
                        i + 1, state.driver, state.driver_ready, state.fault_active, state.fault_latched,
                        state.warning, state.fault);
                } else {
                    out << std::format("m{}: position={:.4f} deg  velocity={:.3f} rpm  referenced={}  switch={}\n",
                        i + 1, state.position, state.velocity, state.referenced ? "yes" : "no",
                        state.switch_active ? "active/open" : "released");
                    out << "    " << (state.feedback == MotorFeedback::Index ? "driver INDEX (pseudo)" : "shaft encoder") << ": ";
                    if (state.position_valid && state.velocity_valid) {
                        out << std::format("position={:.4f} deg  velocity={:.3f} rpm\n", state.actual_position, state.actual_velocity);
                    } else { out << "feedback unavailable\n"; }
                }
            }
            return 0;
        }
    }

    namespace commands
    {
        using namespace pnm::meta::string::literals;
        constexpr auto MOTOR{ Arg{ .name = "motor"_fs, .description = "m1, m2 or m3 (also 1, 2, 3)"_fs } };
        constexpr auto SELECTION{ Arg{ .name = "motor"_fs, .description = "m1, m2, m3 or all (default)"_fs, .optional = true } };
        constexpr auto ACCEL{ Flag{ .name = "accel"_fs, .description = "Acceleration; 0 uses motor default"_fs, .value_name = "deg-s2"_fs } };
        constexpr auto DECEL{ Flag{ .name = "decel"_fs, .description = "Deceleration; 0 uses motor default"_fs, .value_name = "deg-s2"_fs } };
        constexpr auto JERK{ Flag{ .name = "jerk"_fs, .description = "Jerk; 0 uses motor default"_fs, .value_name = "deg-s3"_fs } };
        constexpr auto SPEED{ Flag{ .name = "speed"_fs, .description = "Required positive motor-shaft speed"_fs, .value_name = "rpm"_fs } };
        constexpr auto TIMEOUT{ Flag{ .name = "timeout"_fs, .description = "Execution timeout; 0 means unlimited for moves"_fs, .value_name = "seconds"_fs } };
        constexpr auto BUFFER{ Flag{ .name = "buffer"_fs, .description = "aborting (default), buffered, blending-low, blending-previous, blending-next, blending-high; one queued successor"_fs, .value_name = "mode"_fs } };

        [[= "Motor-shaft control through the IO controller; help motor <command> for options"_fs,
          = Name{ "motor"_fs }]]
        static auto overview(const Context&, const Arguments&, std::ostream& out) -> CallbackResult
        {
            out << "Motor control: motor-shaft degrees and rpm, including m3.\n"
                   "  motor status [m1|m2|m3|all]\n"
                   "  motor driver status [m1|m2|m3|all]\n"
                   "  motor driver init                   Initialize/recover while disabled\n"
                   "  motor enable / motor disable        Shared driver enable\n"
                   "  motor move m1 -90 --speed 5          Relative move\n"
                   "  motor home m1                       Seek, release, slow re-latch\n"
                   "  motor moveto m1 45 --speed 5         Requires successful homing\n"
                   "  motor speed m1 10                   Change active move speed\n"
                   "  motor defaults m1                   Show/change motion dynamics\n"
                   "  motor stop [m1|m2|m3|all]            Abort; retain holding torque\n"
                   "  motor jobs [id]                     Recent 32 jobs and results\n"
                   "Moves and homing return a job ID; the CLI stays available.\n"
                   "Disabling invalidates references. Motors are never enabled automatically.\n";
            return 0;
        }
        [[= "Live commanded/feedback motor-shaft values"_fs, = Name{ "motor status"_fs }, = SELECTION]]
        static auto state(const Context& context, const Arguments& args, std::ostream& out) -> CallbackResult
        { return status(context, args, out, false); }

        [[= "Common driver readiness, alarm and latched fault state"_fs, = Name{ "motor driver status"_fs }, = SELECTION]]
        static auto driverStatus(const Context& context, const Arguments& args, std::ostream& out) -> CallbackResult
        { return status(context, args, out, true); }

        [[= "Initialize/recover all drivers and STEP timebase while disabled"_fs, = Name{ "motor driver init"_fs }]]
        static auto initialize(const Context& context, const Arguments&, std::ostream& out) -> CallbackResult
        {
            context.client->request({ .operation = MotorOperation::Initialize }, context.stop);
            out << "Drivers initialized; STEP timebase ready. Motors remain disabled.\n";
            return 0;
        }
        [[= "Verify and enable all drivers; wait for enable settling"_fs, = Name{ "motor enable"_fs }]]
        static auto enable(const Context& context, const Arguments&, std::ostream& out) -> CallbackResult
        {
            context.client->request({ .operation = MotorOperation::Enable }, context.stop);
            out << "All drivers enabled and settled.\n";
            return 0;
        }
        [[= "Stop all motions, disable holding torque and invalidate references"_fs, = Name{ "motor disable"_fs }]]
        static auto disable(const Context& context, const Arguments&, std::ostream& out) -> CallbackResult
        {
            context.client->request({ .operation = MotorOperation::Disable }, context.stop);
            out << "All drivers disabled; references invalidated.\n";
            return 0;
        }
        [[= "Immediate abort of selected motor and its queue; retains driver enable"_fs, = Name{ "motor stop"_fs }, = SELECTION]]
        static auto stop(const Context& context, const Arguments& args, std::ostream& out) -> CallbackResult
        {
            context.client->request({ .operation = MotorOperation::Stop, .motor = selection(args) }, context.stop);
            out << "Motion stopped; driver enable unchanged.\n";
            return 0;
        }
        [[= "Relative motor-shaft move; negative moves away from reference"_fs, = Name{ "motor move"_fs }, = MOTOR,
          = Arg{ .name = "degrees"_fs, .description = "Signed motor-shaft displacement"_fs },
          = ACCEL, = DECEL, = JERK, = SPEED, = TIMEOUT, = BUFFER]]
        static auto relative(const Context& context, const Arguments& args, std::ostream& out) -> CallbackResult
        { return move(context, args, out, false); }

        [[= "Absolute motor-shaft move; requires successful homing"_fs, = Name{ "motor moveto"_fs }, = MOTOR,
          = Arg{ .name = "degrees"_fs, .description = "Absolute motor-shaft target"_fs },
          = ACCEL, = DECEL, = JERK, = SPEED, = TIMEOUT, = BUFFER]]
        static auto absolute(const Context& context, const Arguments& args, std::ostream& out) -> CallbackResult
        { return move(context, args, out, true); }

        [[= "Abort this motor's motion and home using seek/release/slow latch"_fs, = Name{ "motor home"_fs }, = MOTOR,
          = Flag{ .name = "seek"_fs, .description = "Initial seek speed (default 5)"_fs, .value_name = "rpm"_fs },
          = Flag{ .name = "latch"_fs, .description = "Slow re-latch speed (default 0.5, below seek)"_fs, .value_name = "rpm"_fs }, = TIMEOUT]]
        static auto home(const Context& context, const Arguments& args, std::ostream& out) -> CallbackResult
        {
            const auto seek{ numericOption(args, "seek", 5, false) };
            const auto latch{ numericOption(args, "latch", 0.5, false) };
            if (latch >= seek) { return callback_failure("latch speed must be slower than seek speed"); }
            return submit(context, { .motor = motorId(args.require("motor")), .motion = MotorMotion::Home,
                .velocity = seek, .timeout = numericOption(args, "timeout", 30, false), .latch_velocity = latch }, out);
        }
        [[= "Change an active move's speed while preserving its endpoint and dynamics"_fs, = Name{ "motor speed"_fs }, = MOTOR,
          = Arg{ .name = "rpm"_fs, .description = "Positive speed magnitude"_fs }]]
        static auto speed(const Context& context, const Arguments& args, std::ostream& out) -> CallbackResult
        {
            const auto result{ context.client->request({ .operation = MotorOperation::Speed,
                .motor = motorId(args.require("motor")),
                .velocity = magnitude(number(args.require("rpm"), "speed"), "speed", false) }, context.stop) };
            out << "Speed update scheduled from pulse " << result.pulse << " (1-based).\n";
            return 0;
        }
        [[= "Show/update idle motor defaults; settings live in RAM"_fs, = Name{ "motor defaults"_fs }, = MOTOR,
          = Flag{ .name = "accel"_fs, .description = "Positive default acceleration"_fs, .value_name = "deg-s2"_fs },
          = Flag{ .name = "decel"_fs, .description = "Positive default deceleration"_fs, .value_name = "deg-s2"_fs },
          = Flag{ .name = "jerk"_fs, .description = "Nonnegative default jerk; 0 selects trapezoidal motion"_fs, .value_name = "deg-s3"_fs }]]
        static auto defaults(const Context& context, const Arguments& args, std::ostream& out) -> CallbackResult
        {
            const auto motor{ motorId(args.require("motor")) };
            const auto result{ context.client->request({ .operation = MotorOperation::Defaults, .motor = motor,
                .acceleration = numericOption(args, "accel", 0, false),
                .deceleration = numericOption(args, "decel", 0, false),
                .jerk = numericOption(args, "jerk", 0),
                .dynamics_fields = static_cast<std::uint8_t>((args.hasFlag("accel") ? 1 : 0) |
                  (args.hasFlag("decel") ? 2 : 0) | (args.hasFlag("jerk") ? 4 : 0)) }, context.stop) };
            const auto& state{ result.defaults };
            out << std::format("m{}: accel={} deg/s^2  decel={} deg/s^2  jerk={} deg/s^3\n",
                static_cast<unsigned>(motor) + 1, state.acceleration, state.deceleration, state.jerk);
            return 0;
        }
        [[= "Recent 32 job states; accepted includes running and buffered work"_fs, = Name{ "motor jobs"_fs },
          = Arg{ .name = "id"_fs, .description = "Optional job ID"_fs, .optional = true }]]
        static auto jobs(const Context& context, const Arguments& args, std::ostream& out) -> CallbackResult
        {
            std::optional<std::uint64_t> selected;
            if (const auto text{ args.get("id") }) {
                std::uint64_t value{};
                const auto parsed{ std::from_chars(text->data(), text->data() + text->size(), value) };
                if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size() || value == 0) {
                    return callback_failure("job ID must be a positive integer");
                }
                selected = value;
            }
            bool found{};
            for (const auto& job : context.client->jobs()) {
                if (selected && job.id != *selected) { continue; }
                found = true;
                out << std::format("{} m{} {}: {} | position={:.4f} deg velocity={:.3f} rpm\n",
                    job.id, static_cast<unsigned>(job.motor) + 1,
                    job.motion == MotorMotion::Home ? "home" : job.motion == MotorMotion::Absolute ? "moveto" : "move",
                    jobState(job.state), job.progress.position, job.progress.velocity);
            }
            if (!found && selected) { return callback_failure("job not found (only the most recent 32 are retained)"); }
            if (!found) { out << "No motor jobs.\n"; }
            return 0;
        }
    }

    void setup(Parser& parser, ui::motion::Client& client, std::stop_token stop)
    {
        std::call_once(parser.m_motorRegistration, [&parser, &client, stop] {
            const Context context{ &client, stop };
            register_commands<^^commands>(parser, [context](auto command) {
                return [context, command](const Arguments& args, std::ostream& out) {
                    return command(context, args, out);
                };
            });
        });
    }
}
