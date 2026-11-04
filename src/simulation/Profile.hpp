#pragma once
#include "CadGeometry.hpp"
#include "ui/motion/AxisConversion.hpp"
#include <array>

namespace simulation
{
    struct Profile
    {
        static constexpr const char* NAME{ "fusion-cad-v3" };
        std::array<double, 3> geometry_mm{ cad::LINK_LENGTHS_MM };
        std::array<double, 3> ratio{ cad::MOTOR_REDUCTION };
        double z_mm_per_revolution{ cad::SCREW_LEAD_MM };
        // Straight ahead is the mechanical origin. Z is displacement from CAD rest.
        std::array<double, 3> shaft_reference{};
        std::array<double, 3> axis_reference{};
        std::array<ui::motion::AxisDirection, 3> direction{};
        // Nominal lever flexion; CAD contains no electrical actuation specification.
        // These are lever deflections, NOT joint-angle reference thresholds.
        std::array<double, 3> sensor_operate_deflection_deg{ 2, 2, 2 };
        std::array<double, 3> sensor_release_clearance_mm{ .05, .05, .05 };
        unsigned dm542_microsteps{ 16 };
        void validate() const;
    };
}
