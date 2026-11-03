#pragma once

#include "common/motion/Motor.hpp"
#include <cstdint>
#include <pneumo/messaging.hpp>

namespace icc
{
    enum class ActionProvider : std::uint8_t
    {
        IO,
        UI
    };

    // Shared application contracts. Both bridges register this catalog.
    struct Actions
    {
        [[= ActionProvider::IO]] pnm::msg::
          Action<motion::MotorGoal, motion::MotorProgress, motion::MotorResult>
            io_motor_motion;
    };
}
