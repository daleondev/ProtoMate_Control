#pragma once

#include <cstdint>
#include <pneumo/messaging.hpp>
#include "common/motion/Motor.hpp"

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
        [[= ActionProvider::IO]]
        pnm::msg::Action<motion::MotorGoal, motion::MotorProgress, motion::MotorResult> io_motor_motion;
    };
}
