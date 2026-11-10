#include "Renderer.hpp"
#include "profiling/Trace.hpp"
#include <Renderer3D.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>
namespace ui::gui::scene3d
{
    namespace
    {
        constexpr auto SHADERS{ tgx::SHADER_PERSPECTIVE | tgx::SHADER_ZBUFFER | tgx::SHADER_GOURAUD |
                                tgx::SHADER_NOTEXTURE };
        using Engine = tgx::Renderer3D<tgx::RGB565, SHADERS, std::uint16_t, 1, 0>;
        constexpr std::uint16_t CLEAR_COLOR{ 0x10c4 };
        constexpr float LIGHT_X{ -.4f }, LIGHT_Y{ .6f };
        constexpr int GRID_FIRST{ -6 }, GRID_LAST{ 8 };
        constexpr float GRID_SPACING{ .1f }, GRID_HALF_WIDTH{ .0004f }, GRID_Z{ -.003f };
        constexpr float GRID_MIN{ -.6f }, GRID_MAX{ .8f };
        constexpr float GRID_RED{ .12f }, GRID_GREEN{ .18f }, GRID_BLUE{ .22f };
        constexpr float SPECULAR{ .08f };
        constexpr unsigned SHININESS{ 16 }, CHANNEL_MAX{ 255 }, RED_SHIFT{ 16 }, GREEN_SHIFT{ 8 };
        float& element(tgx::fMat4& m, unsigned row, unsigned column) noexcept
        {
            return m.M[(column * 4) + row];
        }
        void transform(tgx::fMat4& m, const Transform& t) noexcept
        {
            const auto& q{ t.rotation };
            // Column-major rotation followed by uniform scale and translation.
            m.setIdentity();
            element(m, 0, 0) = (1 - 2 * (q.y * q.y + q.z * q.z)) * t.scale;
            element(m, 1, 0) = 2 * (q.x * q.y + q.w * q.z) * t.scale;
            element(m, 2, 0) = 2 * (q.x * q.z - q.w * q.y) * t.scale;
            element(m, 0, 1) = 2 * (q.x * q.y - q.w * q.z) * t.scale;
            element(m, 1, 1) = (1 - 2 * (q.x * q.x + q.z * q.z)) * t.scale;
            element(m, 2, 1) = 2 * (q.y * q.z + q.w * q.x) * t.scale;
            element(m, 0, 2) = 2 * (q.x * q.z + q.w * q.y) * t.scale;
            element(m, 1, 2) = 2 * (q.y * q.z - q.w * q.x) * t.scale;
            element(m, 2, 2) = (1 - 2 * (q.x * q.x + q.y * q.y)) * t.scale;
            element(m, 0, 3) = t.translation.x;
            element(m, 1, 3) = t.translation.y;
            element(m, 2, 3) = t.translation.z;
        }
        void draw_grid(Engine& engine)
        {
            profiling::Scope grid{ PROFILE_SITE_RENDER_GRID, PROFILE_CAT_GRAPHICS };
            tgx::fMat4 identity{};
            identity.setIdentity();
            engine.setModelMatrix(identity);
            engine.setCulling(0);
            engine.setMaterial(tgx::RGBf(GRID_RED, GRID_GREEN, GRID_BLUE), 1.F, 0.F, 0.F, 0);
            const tgx::fVec3 normal{ 0, 0, 1 };
            for (int i{ GRID_FIRST }; i <= GRID_LAST; ++i) {
                const float t{ float(i) * GRID_SPACING };
                engine.drawQuad({ t - GRID_HALF_WIDTH, GRID_MIN, GRID_Z },
                                { t + GRID_HALF_WIDTH, GRID_MIN, GRID_Z },
                                { t + GRID_HALF_WIDTH, GRID_MAX, GRID_Z },
                                { t - GRID_HALF_WIDTH, GRID_MAX, GRID_Z },
                                &normal,
                                &normal,
                                &normal,
                                &normal);
                engine.drawQuad({ GRID_MIN, t - GRID_HALF_WIDTH, GRID_Z },
                                { GRID_MAX, t - GRID_HALF_WIDTH, GRID_Z },
                                { GRID_MAX, t + GRID_HALF_WIDTH, GRID_Z },
                                { GRID_MIN, t + GRID_HALF_WIDTH, GRID_Z },
                                &normal,
                                &normal,
                                &normal,
                                &normal);
            }
        }
        void project_anchors(Engine& engine,
                             const Frame& frame,
                             unsigned node,
                             std::array<ProjectedAnchor, 3>& annotations)
        {
            for (unsigned a{ 0 }; a < frame.anchors.size(); ++a) {
                const auto& anchor{ frame.anchors[a] };
                if (!std::cmp_equal(anchor.node, node)) {
                    continue;
                }
                const tgx::fVec3 point{ anchor.point.x, anchor.point.y, anchor.point.z };
                const auto ndc{ engine.modelToNDC(point) };
                const bool visible{ ndc.w > 0 && std::isfinite(ndc.w) && std::abs(ndc.x) <= 1 &&
                                    std::abs(ndc.y) <= 1 && std::abs(ndc.z) <= 1 };
                annotations[a].value = anchor.value;
                annotations[a].visible = visible;
                if (visible) {
                    const auto pixel{ engine.modelToImage(point) };
                    annotations[a].x = static_cast<std::int16_t>(pixel.x);
                    annotations[a].y = static_cast<std::int16_t>(pixel.y);
                }
            }
        }
    }
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
        if (annotations) {
            *annotations = {};
        }
        static_assert(sizeof(tgx::RGB565) == sizeof(std::uint16_t));
        // TGX RGB565 storage has the asserted 16-bit representation; this is its pixel-buffer boundary.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        tgx::Image<tgx::RGB565> image{ reinterpret_cast<tgx::RGB565*>(color.data()), WIDTH, HEIGHT };
        Engine engine{ { WIDTH, HEIGHT }, &image, depth.data() };
        engine.setShaders(SHADERS);
        const auto clear_start{ std::chrono::steady_clock::now() };
        {
            profiling::Scope clear{ PROFILE_SITE_RENDER_CLEAR, PROFILE_CAT_GRAPHICS };
            image.fillScreen(tgx::RGB565(CLEAR_COLOR));
            engine.clearZbuffer();
        }
        const auto geometry_start{ std::chrono::steady_clock::now() };
        const auto& c{ frame.snapshot.camera };
        const auto horizontal{ c.distance * std::cos(c.pitch) };
        engine.setPerspective(c.field_of_view, float(WIDTH) / HEIGHT, c.near_plane, c.far_plane);
        engine.setLookAt({ c.target.x + (horizontal * std::sin(c.yaw)),
                           c.target.y - (horizontal * std::cos(c.yaw)),
                           c.target.z + (c.distance * std::sin(c.pitch)) },
                         { c.target.x, c.target.y, c.target.z },
                         { 0, 0, 1 });
        engine.setLightDirection({ LIGHT_X, LIGHT_Y, -1 });
        const auto grid_start{ std::chrono::steady_clock::now() };
        if (frame.scene->ground_grid) {
            draw_grid(engine);
        }
        if (timings) {
            timings->grid_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - grid_start)
                                 .count();
        }
        engine.setCulling(1);
        std::array<tgx::fMat4, MAX_NODES> world{};
        for (unsigned i{ 0 }; i < frame.scene->count; ++i) {
            const auto node_start{ std::chrono::steady_clock::now() };
            profiling::Scope node{ i < 4 ? PROFILE_SITE_RENDER_NODE0 + i
                                         : unsigned(PROFILE_SITE_RENDER_OTHER),
                                   PROFILE_CAT_GRAPHICS };
            const auto& n{ frame.scene->nodes[i] };
            const auto& t{ frame.snapshot.transforms[i] };
            auto& m{ world[i] };
            transform(m, t);
            if (n.parent >= 0) {
                m = world[unsigned(n.parent)] * m;
            }
            engine.setModelMatrix(m);
            if (annotations) {
                project_anchors(engine, frame, i, *annotations);
            }

            const auto rgb{ n.material.color };
            engine.setMaterial(tgx::RGBf(float((rgb >> RED_SHIFT) & CHANNEL_MAX) / CHANNEL_MAX,
                                         float((rgb >> GREEN_SHIFT) & CHANNEL_MAX) / CHANNEL_MAX,
                                         float(rgb & CHANNEL_MAX) / CHANNEL_MAX),
                               n.material.ambient,
                               n.material.diffuse,
                               SPECULAR,
                               SHININESS);
            engine.drawMesh(static_cast<const tgx::Mesh3Dv2<tgx::RGB565>*>(n.mesh->data),
                            n.material.use_mesh_material);
            if (timings && i < timings->node_us.size()) {
                timings->node_us[i] = std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - node_start)
                                        .count();
            }
        }
        if (timings) {
            timings->clear_us =
              std::chrono::duration_cast<std::chrono::microseconds>(geometry_start - clear_start).count();
            timings->geometry_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - geometry_start)
                                     .count();
        }
        return true;
    }
}
