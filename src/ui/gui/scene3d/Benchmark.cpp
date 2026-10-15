#include "Scene.hpp"
#include "high.h"
#include "low.h"
#include "medium.h"
#include <cmath>
namespace ui::gui::scene3d
{
    namespace
    {
        const Mesh meshes[]{ { &low, 100, { { -1, -.65f, -.8f }, { 1, .65f, .8f } } },
                             { &medium, 400, { { -1, -.65f, -.8f }, { 1, .65f, .8f } } },
                             { &high, 1000, { { -1, -.65f, -.8f }, { 1, .65f, .8f } } } };
        constexpr Scene scene(unsigned level)
        {
            Scene s;
            s.count = 5;
            constexpr std::uint32_t colors[]{ 0x54dbc3, 0x76a9fa, 0xffba6b, 0xb6a0ff, 0xe87991 };
            for (unsigned i = 0; i < 5; ++i) {
                s.nodes[i] = { &meshes[level], -1, { colors[i] } };
            }
            return s;
        }
        constexpr Scene scenes[]{ scene(0), scene(1), scene(2) };
    }
    const Scene& benchmarkScene(unsigned triangles) noexcept
    {
        return scenes[triangles == 500 ? 0 : triangles == 5000 ? 2 : 1];
    }
    SceneSnapshot benchmarkSnapshot(float phase, std::uint64_t revision, std::uint64_t timeUs) noexcept
    {
        SceneSnapshot s;
        s.revision = revision;
        s.sourceTimeUs = timeUs;
        constexpr Vec3 positions[]{
            { -1.6f, 0, 0 }, { 0, .25f, 0 }, { 1.6f, .5f, 0 }, { -.8f, 1.1f, 1.1f }, { .8f, .9f, 1.1f }
        };
        s.camera.target = { 0, .5f, .3f };
        s.camera.distance = 7;
        for (unsigned i = 0; i < 5; ++i) {
            s.transforms[i].translation = positions[i];
            s.transforms[i].rotation = rotation({ .3f, .5f, 1 }, phase * (.3f + i * .1f));
            s.transforms[i].scale = 1.15f;
        }
        return s;
    }
}
