#pragma once

#include <memory>

namespace control
{
    class MotionController;
}
namespace cli
{
    class Parser;
}

namespace cli::motion
{
    // Register before starting the CLI. Callbacks retain the application-owned
    // controller; no hardware is constructed here. Future robot commands can
    // register a separate group capturing the Robot using this same controller.
    void setup(Parser& parser, std::shared_ptr<control::MotionController> controller);
}
