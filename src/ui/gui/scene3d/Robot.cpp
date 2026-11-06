#include "Robot.hpp"
#include "common/motion/VisualGeometry.hpp"
#include "robot_base_link_asset.hpp"
#include "robot_link_1_asset.hpp"
#include "robot_link_2_asset.hpp"
#include "robot_link_3_asset.hpp"
namespace ui::gui::scene3d
{
    const Scene& robotScene()
    {
        static const Scene scene = [] {
            Scene s{};
            s.count = 4;
            s.groundGrid = true;
            s.nodes[0] = { &assets::robot_base_link, -1 };
            s.nodes[1] = { &assets::robot_link_1, 0 };
            s.nodes[2] = { &assets::robot_link_2, 1 };
            s.nodes[3] = { &assets::robot_link_3, 2 };
            for (unsigned i = 0; i < s.count; ++i)
                s.nodes[i].material.useMeshMaterial = true;
            return s;
        }();
        return scene;
    }
    SceneSnapshot robotSnapshot(const std::array<double, 3>& joints)
    {
        SceneSnapshot result{};
        for (unsigned i = 0; i < 3; ++i) {
            const auto f = ::motion::visual::relative(i, joints[i]);
            result.transforms[i + 1].translation = { float(f.translation[0]),
                                                     float(f.translation[1]),
                                                     float(f.translation[2]) };
            result.transforms[i + 1].rotation = rotation({ 0, 0, 1 }, float(f.yaw));
        }
        result.camera.target = { .20f, 0, .21f };
        result.camera.distance = .95f;
        result.camera.nearPlane = .01f;
        result.camera.farPlane = 4;
        return result;
    }
}
