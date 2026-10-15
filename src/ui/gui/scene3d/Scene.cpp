#include "Scene.hpp"
#include <cmath>
namespace ui::gui::scene3d
{
    namespace
    {
        bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
    }
    bool Scene::valid() const noexcept
    {
        if (!count || count > MAX_NODES) {
            return false;
        }
        for (unsigned i = 0; i < count; ++i) {
            const auto& n = nodes[i];
            // Parent-before-child ordering makes cycle-free evaluation bounded.
            if (n.parent < -1 || n.parent >= int(i) || !n.mesh || !n.mesh->data || !n.mesh->triangles ||
                !finite(n.mesh->bounds.minimum) || !finite(n.mesh->bounds.maximum) ||
                !std::isfinite(n.material.ambient) || !std::isfinite(n.material.diffuse) ||
                n.material.ambient < 0 || n.material.diffuse < 0) {
                return false;
            }
        }
        return true;
    }
    bool SceneSnapshot::valid(const Scene& scene) const noexcept
    {
        const auto& c = camera;
        if (!scene.valid() || !finite(c.target) || !std::isfinite(c.yaw) || !std::isfinite(c.pitch) ||
            !std::isfinite(c.distance) || c.distance <= 0 || !std::isfinite(c.fieldOfView) ||
            c.fieldOfView < 5 || c.fieldOfView > 120 || !std::isfinite(c.nearPlane) ||
            !std::isfinite(c.farPlane) || c.nearPlane <= 0 || c.farPlane <= c.nearPlane ||
            std::abs(c.pitch) > 1.55f) {
            return false;
        }
        for (unsigned i = 0; i < scene.count; ++i) {
            const auto& t = transforms[i];
            const auto& q = t.rotation;
            const auto norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
            if (!finite(t.translation) || !std::isfinite(t.scale) || t.scale <= 0 || !std::isfinite(norm) ||
                std::abs(norm - 1) > .001f) {
                return false;
            }
        }
        return true;
    }
    Quaternion rotation(Vec3 axis, float radians) noexcept
    {
        const auto length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
        if (!(length > 0) || !std::isfinite(length) || !std::isfinite(radians)) {
            return {};
        }
        const auto sine = std::sin(radians * .5f) / length;
        return { axis.x * sine, axis.y * sine, axis.z * sine, std::cos(radians * .5f) };
    }
}
