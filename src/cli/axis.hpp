#pragma once

#include "motion.hpp"

namespace cli::axis
{
    // Joint-coordinate view of the same controller used by cli::motion.
    void setup(Parser& parser, std::shared_ptr<control::MotionController> controller);
}
