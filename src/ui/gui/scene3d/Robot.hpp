#pragma once
#include "Scene.hpp"
#include <array>
namespace ui::gui::scene3d
{
    enum class RobotDetail
    {
        Full,
        Interactive
    };
    const Scene& robotScene(RobotDetail detail = RobotDetail::Full);
    // Joint units: radians, radians, metres. Mesh local origins remain intact.
    SceneSnapshot robotSnapshot(const std::array<double, 3>& joints);
}
