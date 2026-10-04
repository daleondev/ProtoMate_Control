#pragma once

#include "Parser.hpp"
#include "control/Robot.hpp"

namespace cli::robot
{
    void setup(Parser& parser, std::shared_ptr<control::Robot> robot);
}
