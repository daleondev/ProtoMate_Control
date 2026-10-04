#pragma once

#include "Parser.hpp"
#include "control/MotionController.hpp"

namespace cli::motion::detail
{
    double number(std::string_view text, std::string_view name);
    double magnitude(double value, std::string_view name, bool zero = true);
    std::optional<std::string_view> option(const Arguments& args, std::string_view name);
    double numericOption(const Arguments& args, std::string_view name, double fallback, bool zero = true);
    StepperMotor::BufferMode bufferMode(const Arguments& args, std::string_view family = "motor");
    FlagSpec flag(std::string name, std::string description, std::string unit);
    std::string_view axisName(control::MotionController::MotorId motor);
    std::string_view stateName(hal::step::State state);
    int submitted(const control::MotionController::Motion& value,
                  std::ostream& out,
                  std::string_view family = "motor");
    CallbackResult jobs(const Arguments& args,
                        std::ostream& out,
                        control::MotionController& controller,
                        std::string_view family);
}
