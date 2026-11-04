#include "assets/EmbeddedAssets.hpp"

namespace simulation::assets
{
    namespace
    {
        // Supplied by CMake after the private ThreadX controller has linked.
        alignas(16) constinit const unsigned char CONTROLLER[]{
#embed PROTOMATE_CONTROLLER_IMAGE
        };
        static_assert(sizeof(CONTROLLER) >= 64, "embedded controller image must contain an ELF header");
    }
    std::span<const std::byte> controllerImage() noexcept { return std::as_bytes(std::span{ CONTROLLER }); }
}
