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

namespace cli::axis
{
    // Joint-coordinate view of the same controller used by cli::motor.
    void setup(Parser& parser, std::shared_ptr<control::MotionController> controller);
}
