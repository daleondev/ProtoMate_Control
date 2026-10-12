#pragma once

#include <memory>

namespace control
{
    class ControllerClient;
}
namespace cli
{
    class Parser;
}

namespace cli::motor
{
    // Register before starting the CLI. Callbacks retain the application-owned
    // controller; no hardware is constructed here.
    void setup(Parser& parser, const std::shared_ptr<control::ControllerClient>& controller);
}
