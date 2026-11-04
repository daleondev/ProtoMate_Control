#pragma once
#include "Profile.hpp"
#include "ui/motion/AxisConversion.hpp"
#include <glm/glm.hpp>

namespace simulation
{
    // Native mechanics use the SAME pure conversion implementation as robot planning.
    // Shaft truth is independent of controller/encoder coordinate resets.
    class Kinematics
    {
      public:
        explicit Kinematics(const Profile& profile);
        std::array<double, 3> joints(const std::array<double, 3>& shaft_degrees) const;
        static std::array<double, 3> tcp(const Profile&, const std::array<double, 3>& joints);
        static glm::dmat4 relativeFrame(const Profile&, unsigned axis, double joint);
        static std::array<glm::dmat4, 4> frames(const Profile&, const std::array<double, 3>& joints);

      private:
        std::array<ui::motion::RotaryAxisConversion, 2> m_rotary;
        ui::motion::LinearAxisConversion m_linear;
    };
}
