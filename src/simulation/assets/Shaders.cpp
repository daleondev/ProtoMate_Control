#include "EmbeddedAssets.hpp"

namespace simulation::assets
{
    namespace
    {
        // These arrays deliberately have no trailing NUL; GL receives exact lengths.
        constexpr char SURFACE_VERTEX[]{
#embed "shaders/surface.vert"
        };
        constexpr char SCENE_FRAGMENT[]{
#embed "shaders/scene.frag"
        };
        constexpr char MODEL_FRAGMENT[]{
#embed "shaders/model.frag"
        };
    }
    ShaderSources sceneShader() noexcept
    {
        return { { SURFACE_VERTEX, sizeof(SURFACE_VERTEX) }, { SCENE_FRAGMENT, sizeof(SCENE_FRAGMENT) } };
    }
    ShaderSources modelShader() noexcept
    {
        return { { SURFACE_VERTEX, sizeof(SURFACE_VERTEX) }, { MODEL_FRAGMENT, sizeof(MODEL_FRAGMENT) } };
    }
}
