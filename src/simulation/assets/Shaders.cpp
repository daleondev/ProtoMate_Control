#include "EmbeddedAssets.hpp"

namespace simulation::assets
{
    namespace
    {
        // These arrays deliberately have no trailing NUL; GL receives exact lengths.
        // #embed determines the bound; keep the embedded bytes in one static array.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
        constexpr char SURFACE_VERTEX[]{
#embed "shaders/surface.vert"
        };
        // #embed determines the bound; keep the embedded bytes in one static array.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
        constexpr char SCENE_FRAGMENT[]{
#embed "shaders/scene.frag"
        };
        // #embed determines the bound; keep the embedded bytes in one static array.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
        constexpr char MODEL_FRAGMENT[]{
#embed "shaders/model.frag"
        };
    }
    ShaderSources scene_shader() noexcept
    {
        return { .vertex = { &SURFACE_VERTEX[0], sizeof(SURFACE_VERTEX) },
                 .fragment = { &SCENE_FRAGMENT[0], sizeof(SCENE_FRAGMENT) } };
    }
    ShaderSources model_shader() noexcept
    {
        return { .vertex = { &SURFACE_VERTEX[0], sizeof(SURFACE_VERTEX) },
                 .fragment = { &MODEL_FRAGMENT[0], sizeof(MODEL_FRAGMENT) } };
    }
}
