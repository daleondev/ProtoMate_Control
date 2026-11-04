#pragma once
#include <array>

namespace simulation::cad
{
    // Nominal evaluated STEP geometry, millimetres. See docs/robot/geometry.md
    // and geometry.json for source hashes, measurement evidence and limitations.
    inline constexpr std::array<double, 3> LINK_LENGTHS_MM{ 205.0, 223.4, 135.0 };
    inline constexpr double J1_FRAME_Z_MM{ 52.0 };
    inline constexpr double J2_FRAME_Z_MM{ 369.9 };
    inline constexpr double CARRIAGE_FRAME_Z_MM{ 308.9 };
    inline constexpr double TOOL_TIP_LOCAL_Z_MM{ -280.0 };
    inline constexpr double REST_TCP_Z_MM{ 28.9 };
    // Tooth counts are nominal component metadata. Screw pitch is explicitly
    // assumed by the CAD author; the STEP contains a smooth thread envelope.
    inline constexpr std::array<double, 3> MOTOR_REDUCTION{ 5.0, 5.0, 1.0 };
    inline constexpr double SCREW_LEAD_MM{ 1.25 };

    enum class Link : unsigned
    {
        Base,
        Shoulder,
        Forearm,
        Carriage
    };
    struct SensorMount
    {
        Link parent{};
        std::array<double, 3> plunger_center_mm{};
        std::array<double, 3> outward_axis{};
    };
    // Positions in the corresponding DAE link frame, at the supplier's free
    // plunger position. These are NOT electrical trigger points/home offsets.
    inline constexpr std::array<SensorMount, 3> HOME_SENSOR_MOUNTS{
        { { Link::Base, { -78.7, 0.4, 195.2 }, { 1, 0, 0 } },
          { Link::Shoulder, { 154.1, 0.4, 221.8 }, { 1, 0, 0 } },
          { Link::Forearm, { 321.6, 49.7, -106.5 }, { 0, -1, 0 } } }
    };
}
