#include "assets/EmbeddedAssets.hpp"

namespace simulation::assets
{
    namespace
    {
        // Supplied by CMake after the private ThreadX controller has linked.
        // #embed determines the bound; keep the embedded bytes in one static array.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
        alignas(16) constinit const unsigned char CONTROLLER[]{
// ELF data intentionally contains NUL bytes, not a C string.
// NOLINTNEXTLINE(bugprone-string-literal-with-embedded-nul)
#embed PROTOMATE_CONTROLLER_IMAGE
        };
        constexpr std::size_t ELF64_HEADER_BYTES{ 64 };
        static_assert(sizeof(CONTROLLER) >= ELF64_HEADER_BYTES,
                      "embedded controller image must contain an ELF header");
    }
    std::span<const std::byte> controller_image() noexcept { return std::as_bytes(std::span{ CONTROLLER }); }
}
