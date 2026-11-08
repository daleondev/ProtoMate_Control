#pragma once
#include "Profile.hpp"
#include <array>
#include <limits>

namespace simulation
{
    class ContactSensors
    {
      public:
        explicit ContactSensors(const Profile& profile);
        bool update(unsigned axis, double joint);
        // Geometry oracle for verification and diagnostics. No command/homing state.
        static double clearance(const Profile&, unsigned axis, double joint, double lever_deflection_degrees);
        static bool contact(const Profile&, unsigned axis, double joint, double lever_deflection_degrees);
        struct Boundary
        {
            double operate{}, release{}; // Radians for rotary axes, millimetres for Axis 3.
        };
        // First cam encountered from the CAD zero, on the requested side.
        static Boundary boundary(const Profile&, unsigned axis, bool positive);

      private:
        Profile m_profile;
        std::array<bool, 3> m_active{};
        std::array<double, 3> m_margin{}, m_radius{};
        // Cache only while a geometric displacement bound proves the state cannot change.
        // m_previous is the last EXACT query pose, not the previous pulse pose.
        std::array<double, 3> m_previous{ std::numeric_limits<double>::quiet_NaN(),
                                          std::numeric_limits<double>::quiet_NaN(),
                                          std::numeric_limits<double>::quiet_NaN() };
    };
}
