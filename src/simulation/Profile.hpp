#pragma once
#include "CadGeometry.hpp"
#include <array>

namespace simulation
{
    struct Profile
    {
        static constexpr const char* NAME{ "fusion-cad-v2" };
        std::array<double, 3> geometry_mm{ cad::LINK_LENGTHS_MM };
        std::array<double, 3> ratio{ cad::MOTOR_REDUCTION };
        double z_mm_per_revolution{ cad::SCREW_LEAD_MM };
        // Uncalibrated homing fixtures, deliberately independent of CAD sensor mounts.
        std::array<double, 3> shaft_reference{ 135.0, 135.0, 135.0 };
        std::array<double, 3> axis_reference{ 0.0, 1.2, 30.0 };
        double switch_on{ 135.0 }, switch_off{ 134.55 };
        unsigned dm542_microsteps{ 16 };
        void validate() const;
    };
}
