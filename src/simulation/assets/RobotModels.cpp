#include "EmbeddedAssets.hpp"

namespace simulation::assets
{
    namespace
    {
        constinit const unsigned char BASE[]{
#embed "robot/base_link/base_link.dae"
        };
        constinit const unsigned char SHOULDER[]{
#embed "robot/link_1/link_1.dae"
        };
        constinit const unsigned char FOREARM[]{
#embed "robot/link_2/link_2.dae"
        };
        constinit const unsigned char CARRIAGE[]{
#embed "robot/link_3/link_3.dae"
        };
    }
    std::array<std::span<const std::byte>, 4> robotModels() noexcept
    {
        return { std::as_bytes(std::span{ BASE }),
                 std::as_bytes(std::span{ SHOULDER }),
                 std::as_bytes(std::span{ FOREARM }),
                 std::as_bytes(std::span{ CARRIAGE }) };
    }
}
