#include "control_common.hpp"

#include <charconv>
#include <cmath>
#include <format>
#include <ostream>
#include <stdexcept>

namespace cli::control_detail
{
    using Controller = control::MotionController;
    using MotorId = Controller::MotorId;

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
        const auto label{ family == "axis" ? std::string(axisName(value.motor))
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

}
