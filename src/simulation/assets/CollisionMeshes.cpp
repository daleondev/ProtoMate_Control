#include "EmbeddedAssets.hpp"
namespace simulation::assets
{
    namespace
    {
        constinit const unsigned char LINK1[]{
#embed "robot/sensors/j1_cams.stl"
        };
        constinit const unsigned char LINK2[]{
#embed "robot/sensors/j2_cams.stl"
        };
        constinit const unsigned char LINK3[]{
#embed "robot/link_3/link_3_collision.stl"
        };
        constinit const unsigned char J1[]{
#embed "robot/sensors/j1_lever.stl"
        };
        constinit const unsigned char J2[]{
#embed "robot/sensors/j2_lever.stl"
        };
        constinit const unsigned char Z[]{
#embed "robot/sensors/z_lever.stl"
        };
    }
    std::array<std::span<const std::byte>, 3> collisionLinks() noexcept
    {
        return { std::as_bytes(std::span{ LINK1 }),
                 std::as_bytes(std::span{ LINK2 }),
                 std::as_bytes(std::span{ LINK3 }) };
    }
    std::array<std::span<const std::byte>, 3> switchLevers() noexcept
    {
        return { std::as_bytes(std::span{ J1 }),
                 std::as_bytes(std::span{ J2 }),
                 std::as_bytes(std::span{ Z }) };
    }
}
