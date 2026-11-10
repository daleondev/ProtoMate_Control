#include "Robot.hpp"
#include "common/motion/VisualGeometry.hpp"
#include "robot_base_link_asset.hpp"
#include "robot_interactive_base_link_asset.hpp"
#include "robot_interactive_link_1_asset.hpp"
#include "robot_interactive_link_2_asset.hpp"
#include "robot_interactive_link_3_asset.hpp"
#include "robot_link_1_asset.hpp"
#include "robot_link_2_asset.hpp"
#include "robot_link_3_asset.hpp"
namespace ui::gui::scene3d
{
    const Scene& robot_scene(RobotDetail detail)
    {
        static constexpr auto scenes{ [] {
            Scene s{};
            s.count = 4;
            s.ground_grid = true;
            s.nodes[0] = { .mesh = &assets::robot_base_link, .parent = -1 };
            s.nodes[1] = { .mesh = &assets::robot_link_1, .parent = 0 };
            s.nodes[2] = { .mesh = &assets::robot_link_2, .parent = 1 };
            s.nodes[3] = { .mesh = &assets::robot_link_3, .parent = 2 };
            for (unsigned i{ 0 }; i < s.count; ++i) {
                s.nodes[i].material.use_mesh_material = true;
            }
            auto interactive{ s };
            interactive.nodes[0].mesh = &assets::robot_interactive_base_link;
            interactive.nodes[1].mesh = &assets::robot_interactive_link_1;
            interactive.nodes[2].mesh = &assets::robot_interactive_link_2;
            interactive.nodes[3].mesh = &assets::robot_interactive_link_3;
            return std::array{ s, interactive };
        }() };
        return scenes[detail == RobotDetail::Interactive ? 1 : 0];
    }
    namespace
    {
        constexpr Vec3 ROBOT_CAMERA_TARGET{ .x = .20f, .y = 0, .z = .21f };
        constexpr float ROBOT_CAMERA_DISTANCE{ .95f }, ROBOT_NEAR_PLANE{ .01f };
    }
    SceneSnapshot robot_snapshot(const std::array<double, 3>& joints)
    {
        SceneSnapshot result{};
        for (unsigned i{ 0 }; i < 3; ++i) {
            const auto f{ ::motion::visual::relative(i, joints[i]) };
            result.transforms[i + 1].translation = { .x = float(f.translation[0]),
                                                     .y = float(f.translation[1]),
                                                     .z = float(f.translation[2]) };
            result.transforms[i + 1].rotation = rotation({ .x = 0, .y = 0, .z = 1 }, float(f.yaw));
        }
        result.camera.target = ROBOT_CAMERA_TARGET;
        result.camera.distance = ROBOT_CAMERA_DISTANCE;
        result.camera.near_plane = ROBOT_NEAR_PLANE;
        result.camera.far_plane = 4;
        return result;
    }
}
