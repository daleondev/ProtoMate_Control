#include "Scene.hpp"
#include <cmath>
#include <utility>
namespace ui::gui::scene3d
{
    namespace
    {
        constexpr float MIN_FOV{ 5 }, MAX_FOV{ 120 }, MAX_PITCH{ 1.55F }, UNIT_QUATERNION_TOLERANCE{ .001f },
          HALF{ .5f };
        bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
    }
    bool Scene::valid() const noexcept
    {
        if (!count || count > MAX_NODES) {
            return false;
        }
        for (unsigned i{ 0 }; i < count; ++i) {
            const auto& n{ nodes[i] };
            // Parent-before-child ordering makes cycle-free evaluation bounded.
            if (n.parent < -1 || std::cmp_greater_equal(n.parent, i) || !n.mesh || !n.mesh->data ||
                !n.mesh->triangles || !finite(n.mesh->bounds.minimum) || !finite(n.mesh->bounds.maximum) ||
                !std::isfinite(n.material.ambient) || !std::isfinite(n.material.diffuse) ||
                n.material.ambient < 0 || n.material.diffuse < 0) {
                return false;
            }
        }
        return true;
    }
    bool SceneSnapshot::valid(const Scene& scene) const noexcept
    {
        const auto& c{ camera };
        if (!scene.valid() || !finite(c.target) || !std::isfinite(c.yaw) || !std::isfinite(c.pitch) ||
            !std::isfinite(c.distance) || c.distance <= 0 || !std::isfinite(c.field_of_view) ||
            c.field_of_view < MIN_FOV || c.field_of_view > MAX_FOV || !std::isfinite(c.near_plane) ||
            !std::isfinite(c.far_plane) || c.near_plane <= 0 || c.far_plane <= c.near_plane ||
            std::abs(c.pitch) > MAX_PITCH) {
            return false;
        }
        for (unsigned i{ 0 }; i < scene.count; ++i) {
            const auto& t{ transforms[i] };
            const auto& q{ t.rotation };
            const auto norm{ (q.x * q.x) + (q.y * q.y) + (q.z * q.z) + (q.w * q.w) };
            if (!finite(t.translation) || !std::isfinite(t.scale) || t.scale <= 0 || !std::isfinite(norm) ||
                std::abs(norm - 1) > UNIT_QUATERNION_TOLERANCE) {
                return false;
            }
        }
        return true;
    }
    Quaternion rotation(Vec3 axis, float radians) noexcept
    {
        const auto length{ std::sqrt((axis.x * axis.x) + (axis.y * axis.y) + (axis.z * axis.z)) };
        if (!(length > 0) || !std::isfinite(length) || !std::isfinite(radians)) {
            return {};
        }
        const auto sine{ std::sin(radians * HALF) / length };
        return { .x = axis.x * sine, .y = axis.y * sine, .z = axis.z * sine, .w = std::cos(radians * HALF) };
    }
}
