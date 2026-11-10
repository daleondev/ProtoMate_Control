#include "Scene.hpp"
#include "high.h"
#include "low.h"
#include "medium.h"
#include <array>
#include <cmath>
namespace ui::gui::scene3d
{
    namespace
    {
        constexpr std::array<Mesh, 3> MESHES{
            { { .data = &low,
                .triangles = 100,
                .bounds = { .minimum = { .x = -1, .y = -.65f, .z = -.8f },
                            .maximum = { .x = 1, .y = .65f, .z = .8f } } },
              { .data = &medium,
                .triangles = 400,
                .bounds = { .minimum = { .x = -1, .y = -.65f, .z = -.8f },
                            .maximum = { .x = 1, .y = .65f, .z = .8f } } },
              { .data = &high,
                .triangles = 1000,
                .bounds = { .minimum = { .x = -1, .y = -.65f, .z = -.8f },
                            .maximum = { .x = 1, .y = .65f, .z = .8f } } } }
        };
        constexpr unsigned NODE_COUNT{ 5 }, SMALL_TRIANGLES{ 500 }, LARGE_TRIANGLES{ 5000 };
        constexpr Vec3 CAMERA_TARGET{ .x = 0, .y = .5f, .z = .3f },
          ROTATION_AXIS{ .x = .3f, .y = .5f, .z = 1 };
        constexpr float CAMERA_DISTANCE{ 7 }, BASE_RATE{ .3f }, RATE_STEP{ .1f }, SCALE{ 1.15F };
        constexpr Scene scene(unsigned level)
        {
            Scene s{};
            s.count = NODE_COUNT;
            constexpr std::array<std::uint32_t, 5> colors{ 0x54dbc3, 0x76a9fa, 0xffba6b, 0xb6a0ff, 0xe87991 };
            for (unsigned i{ 0 }; i < NODE_COUNT; ++i) {
                s.nodes[i] = { .mesh = &MESHES[level], .parent = -1, .material = { .color = colors[i] } };
            }
            return s;
        }
        constexpr std::array SCENES{ scene(0), scene(1), scene(2) };
    }
    const Scene& benchmark_scene(unsigned triangles) noexcept
    {
        if (triangles == SMALL_TRIANGLES) {
            return SCENES[0];
        }
        if (triangles == LARGE_TRIANGLES) {
            return SCENES[2];
        }
        return SCENES[1];
    }
    SceneSnapshot benchmark_snapshot(float phase, SnapshotStamp stamp) noexcept
    {
        SceneSnapshot s{};
        s.revision = stamp.revision;
        s.source_time_us = stamp.time_us;
        constexpr std::array<Vec3, 5> positions{ { { .x = -1.6F, .y = 0, .z = 0 },
                                                   { .x = 0, .y = .25f, .z = 0 },
                                                   { .x = 1.6F, .y = .5f, .z = 0 },
                                                   { .x = -.8f, .y = 1.1F, .z = 1.1F },
                                                   { .x = .8f, .y = .9f, .z = 1.1F } } };
        s.camera.target = CAMERA_TARGET;
        s.camera.distance = CAMERA_DISTANCE;
        for (unsigned i{ 0 }; i < NODE_COUNT; ++i) {
            s.transforms[i].translation = positions[i];
            s.transforms[i].rotation =
              rotation(ROTATION_AXIS, phase * (BASE_RATE + static_cast<float>(i) * RATE_STEP));
            s.transforms[i].scale = SCALE;
        }
        return s;
    }
}
