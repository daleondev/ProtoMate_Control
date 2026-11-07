#include "Renderer.hpp"
#include "profiling/Trace.hpp"
#include <Renderer3D.h>
#include <algorithm>
#include <chrono>
#include <cmath>
namespace ui::gui::scene3d
{
    bool render(const Frame& frame,
                std::span<std::uint16_t> color,
                std::span<std::uint16_t> depth,
                RenderTimings* timings,
                std::array<ProjectedAnchor, 3>* annotations) noexcept
    {
        profiling::Scope measurement{ PROFILE_SITE_RENDER, PROFILE_CAT_GRAPHICS };
        if (!frame.scene || !frame.snapshot.valid(*frame.scene) || color.size() < PIXELS ||
            depth.size() < PIXELS) {
            return false;
        }
        if (annotations)
            *annotations = {};
        constexpr auto shaders{ tgx::SHADER_PERSPECTIVE | tgx::SHADER_ZBUFFER | tgx::SHADER_GOURAUD |
                                tgx::SHADER_NOTEXTURE };
        using Engine = tgx::Renderer3D<tgx::RGB565, shaders, std::uint16_t, 1, 0>;
        static_assert(sizeof(tgx::RGB565) == sizeof(std::uint16_t));
        tgx::Image<tgx::RGB565> image{ reinterpret_cast<tgx::RGB565*>(color.data()), WIDTH, HEIGHT };
        Engine engine{ { WIDTH, HEIGHT }, &image, depth.data() };
        engine.setShaders(shaders);
        const auto clearStart{ std::chrono::steady_clock::now() };
        {
            profiling::Scope clear{ PROFILE_SITE_RENDER_CLEAR, PROFILE_CAT_GRAPHICS };
            image.fillScreen(tgx::RGB565(std::uint16_t{ 0x10c4 }));
            engine.clearZbuffer();
        }
        const auto geometryStart{ std::chrono::steady_clock::now() };
        const auto& c{ frame.snapshot.camera };
        const auto horizontal{ c.distance * std::cos(c.pitch) };
        engine.setPerspective(c.fieldOfView, float(WIDTH) / HEIGHT, c.nearPlane, c.farPlane);
        engine.setLookAt({ c.target.x + horizontal * std::sin(c.yaw),
                           c.target.y - horizontal * std::cos(c.yaw),
                           c.target.z + c.distance * std::sin(c.pitch) },
                         { c.target.x, c.target.y, c.target.z },
                         { 0, 0, 1 });
        engine.setLightDirection({ -.4f, .6f, -1 });
        const auto gridStart = std::chrono::steady_clock::now();
        if (frame.scene->groundGrid) {
            profiling::Scope grid{ PROFILE_SITE_RENDER_GRID, PROFILE_CAT_GRAPHICS };
            tgx::fMat4 identity;
            identity.setIdentity();
            engine.setModelMatrix(identity);
            engine.setCulling(0);
            engine.setMaterial(tgx::RGBf(.12f, .18f, .22f), 1.f, 0.f, 0.f, 0);
            const tgx::fVec3 normal{ 0, 0, 1 };
            for (int i = -6; i <= 8; ++i) {
                const float t = float(i) * .1f, h = .0004f, z = -.003f;
                engine.drawQuad({ t - h, -.6f, z },
                                { t + h, -.6f, z },
                                { t + h, .8f, z },
                                { t - h, .8f, z },
                                &normal,
                                &normal,
                                &normal,
                                &normal);
                engine.drawQuad({ -.6f, t - h, z },
                                { .8f, t - h, z },
                                { .8f, t + h, z },
                                { -.6f, t + h, z },
                                &normal,
                                &normal,
                                &normal,
                                &normal);
            }
        }
        if (timings)
            timings->gridUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - gridStart)
                                .count();
        engine.setCulling(1);
        std::array<tgx::fMat4, MAX_NODES> world{};
        for (unsigned i{ 0 }; i < frame.scene->count; ++i) {
            const auto nodeStart = std::chrono::steady_clock::now();
            profiling::Scope node{ i < 4 ? PROFILE_SITE_RENDER_NODE0 + i
                                         : unsigned(PROFILE_SITE_RENDER_OTHER),
                                   PROFILE_CAT_GRAPHICS };
            const auto& n{ frame.scene->nodes[i] };
            const auto& t{ frame.snapshot.transforms[i] };
            const auto& q{ t.rotation };
            auto& m{ world[i] };
            // Column-major rotation followed by uniform scale and translation.
            m.setIdentity();
            m.M[0] = (1 - 2 * (q.y * q.y + q.z * q.z)) * t.scale;
            m.M[1] = 2 * (q.x * q.y + q.w * q.z) * t.scale;
            m.M[2] = 2 * (q.x * q.z - q.w * q.y) * t.scale;
            m.M[4] = 2 * (q.x * q.y - q.w * q.z) * t.scale;
            m.M[5] = (1 - 2 * (q.x * q.x + q.z * q.z)) * t.scale;
            m.M[6] = 2 * (q.y * q.z + q.w * q.x) * t.scale;
            m.M[8] = 2 * (q.x * q.z + q.w * q.y) * t.scale;
            m.M[9] = 2 * (q.y * q.z - q.w * q.x) * t.scale;
            m.M[10] = (1 - 2 * (q.x * q.x + q.y * q.y)) * t.scale;
            m.M[12] = t.translation.x;
            m.M[13] = t.translation.y;
            m.M[14] = t.translation.z;
            if (n.parent >= 0) {
                m = world[unsigned(n.parent)] * m;
            }
            engine.setModelMatrix(m);
            if (annotations)
                for (unsigned a = 0; a < frame.anchors.size(); ++a) {
                    const auto& anchor = frame.anchors[a];
                    if (anchor.node != int(i))
                        continue;
                    const tgx::fVec3 point{ anchor.point.x, anchor.point.y, anchor.point.z };
                    const auto ndc = engine.modelToNDC(point);
                    const bool visible = ndc.w > 0 && std::isfinite(ndc.w) && std::abs(ndc.x) <= 1 &&
                                         std::abs(ndc.y) <= 1 && std::abs(ndc.z) <= 1;
                    (*annotations)[a].value = anchor.value;
                    (*annotations)[a].visible = visible;
                    if (visible) {
                        const auto pixel = engine.modelToImage(point);
                        (*annotations)[a].x = static_cast<std::int16_t>(pixel.x);
                        (*annotations)[a].y = static_cast<std::int16_t>(pixel.y);
                    }
                }

            const auto rgb{ n.material.color };
            engine.setMaterial(tgx::RGBf(float((rgb >> 16) & 255) / 255,
                                         float((rgb >> 8) & 255) / 255,
                                         float(rgb & 255) / 255),
                               n.material.ambient,
                               n.material.diffuse,
                               .08f,
                               16);
            engine.drawMesh(static_cast<const tgx::Mesh3Dv2<tgx::RGB565>*>(n.mesh->data),
                            n.material.useMeshMaterial);
            if (timings && i < timings->nodeUs.size())
                timings->nodeUs[i] = std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - nodeStart)
                                       .count();
        }
        if (timings) {
            timings->clearUs =
              std::chrono::duration_cast<std::chrono::microseconds>(geometryStart - clearStart).count();
            timings->geometryUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - geometryStart)
                                    .count();
        }
        return true;
    }
}
