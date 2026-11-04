#include "EmbeddedAssets.hpp"

namespace simulation::assets
{
    namespace
    {
        constexpr unsigned char SHOULDER[]{
#embed "robot/shoulder.glb"
        };
        constexpr unsigned char FOREARM[]{
#embed "robot/forearm.glb"
        };
        constexpr unsigned char CARRIAGE[]{
#embed "robot/carriage.glb"
        };
    }
    std::array<std::span<const std::byte>, 3> robotModels() noexcept
    {
        return { std::as_bytes(std::span{ SHOULDER }),
                 std::as_bytes(std::span{ FOREARM }),
                 std::as_bytes(std::span{ CARRIAGE }) };
    }
}
