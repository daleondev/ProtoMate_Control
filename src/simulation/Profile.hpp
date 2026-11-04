#pragma once
#include <array>

namespace simulation
{
    struct Profile
    {
        static constexpr const char* NAME{ "cad-provisional-v1" };
        std::array<double, 3> geometry_mm{ 205.0, 223.4, 135.0 };
        std::array<double, 3> ratio{ 6.0, 6.0, 1.0 };
        double z_mm_per_revolution{ 8.0 };
        std::array<double, 3> shaft_reference{ 135.0, 135.0, 135.0 };
        std::array<double, 3> axis_reference{ 0.0, 1.2, 30.0 };
        double switch_on{ 135.0 }, switch_off{ 134.55 };
        unsigned dm542_microsteps{ 16 };
        void validate() const;
    };
}
