#pragma once
#include "Scene.hpp"
#include <array>
#include <cstdint>
namespace ui::gui::scene3d
{
    enum class RobotDetail : std::uint8_t
    {
        Full,
        Interactive
    };
    const Scene& robot_scene(RobotDetail detail = RobotDetail::Full);
    // Joint units: radians, radians, metres. Mesh local origins remain intact.
    SceneSnapshot robot_snapshot(const std::array<double, 3>& joints);
}
