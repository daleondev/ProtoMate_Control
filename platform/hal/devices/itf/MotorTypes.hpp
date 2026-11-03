#pragma once

#include <cstddef>
#include <cstdint>
#include <pneumo/units.hpp>

namespace hal::motor
{
    enum class Id : std::uint8_t
    {
        Motor1,
        Motor2,
        Motor3
    };

    struct FeedbackConfiguration
    {
        pnm::units::Angle full_step_angle;
        std::size_t microsteps{};
    };
}
