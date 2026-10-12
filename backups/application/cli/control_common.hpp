#pragma once

#include "Parser.hpp"
#include "control/ControllerClient.hpp"

namespace cli::control_detail
{
    double number(std::string_view text, std::string_view name);
    double magnitude(double value, std::string_view name, bool zero = true);
    std::optional<std::string_view> option(const Arguments& args, std::string_view name);
    double numeric_option(const Arguments& args, std::string_view name, double fallback, bool zero = true);
    control::BufferMode buffer_mode(const Arguments& args, std::string_view family = "motor");
    std::string_view axis_name(control::ControllerClient::MotorId motor);
    std::string_view state_name(hal::step::State state);
    int submitted(const control::ControllerClient::Motion& value,
                  std::ostream& out,
                  std::string_view family = "motor");
    CallbackResult jobs(const Arguments& args,
                        std::ostream& out,
                        control::ControllerClient& controller,
                        std::string_view family);
}
