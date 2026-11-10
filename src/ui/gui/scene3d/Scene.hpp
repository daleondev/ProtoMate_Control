#pragma once
#include "RenderPacing.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace ui::gui::scene3d
{
    inline constexpr unsigned WIDTH{ 640 }, HEIGHT{ 400 }, MAX_NODES{ 32 };
    inline constexpr std::size_t PIXELS{ static_cast<std::size_t>(WIDTH) * HEIGHT };
    struct Vec3
    {
        float x{}, y{}, z{};
    };
    struct Quaternion
    {
        float x{}, y{}, z{}, w{ 1 };
    };
    struct Transform
    {
        Vec3 translation{};
        Quaternion rotation{};
        float scale{ 1 };
    };
    struct Bounds
    {
        Vec3 minimum{}, maximum{};
    };
    // Generated, immutable TGX meshlet data. Assets must outlive every submitted
    // scene. Its representation is confined to the renderer/converter adapter.
    struct Mesh
    {
        const void* data{};
        unsigned triangles{};
        Bounds bounds{};
    };
    inline constexpr std::uint32_t DEFAULT_MATERIAL_COLOR{ 0x54dbc3 };
    inline constexpr float DEFAULT_AMBIENT{ .25f }, DEFAULT_DIFFUSE{ .7f };
    inline constexpr float DEFAULT_YAW{ .55f }, DEFAULT_PITCH{ .45f }, DEFAULT_DISTANCE{ 7.5F };
    inline constexpr float DEFAULT_FIELD_OF_VIEW{ 40 }, DEFAULT_NEAR_PLANE{ .1f }, DEFAULT_FAR_PLANE{ 50 };
    struct Material
    {
        std::uint32_t color{ DEFAULT_MATERIAL_COLOR };
        float ambient{ DEFAULT_AMBIENT }, diffuse{ DEFAULT_DIFFUSE };
        // Opt in to OBJ/MTL colours without a separate node for each material.
        bool use_mesh_material{ false };
    };
    struct Node
    {
        const Mesh* mesh{};
        int parent{ -1 };
        Material material{};
    };
    struct Scene
    {
        std::array<Node, MAX_NODES> nodes{};
        unsigned count{};
        bool ground_grid{};
        bool valid() const noexcept;
    };
    struct Camera
    {
        Vec3 target{};
        float yaw{ DEFAULT_YAW }, pitch{ DEFAULT_PITCH },
          distance{ DEFAULT_DISTANCE }; // radians; right-handed Z-up
        float field_of_view{ DEFAULT_FIELD_OF_VIEW }, near_plane{ DEFAULT_NEAR_PLANE },
          far_plane{ DEFAULT_FAR_PLANE };
    };
    struct SceneSnapshot
    {
        std::array<Transform, MAX_NODES> transforms{};
        Camera camera{};
        std::uint64_t revision{}, source_time_us{};
        bool valid(const Scene& scene) const noexcept;
    };
    struct Anchor
    {
        int node{ -1 };
        Vec3 point{};
        float value{};
    };
    struct ProjectedAnchor
    {
        std::int16_t x{}, y{};
        float value{};
        bool visible{};
    };
    struct Frame
    {
        const Scene* scene{};
        SceneSnapshot snapshot{};
        unsigned maximum_fps{ LIVE_MAX_FPS };
        std::array<Anchor, 3> anchors{};
    };
    Quaternion rotation(Vec3 axis, float radians) noexcept;
    const Scene& benchmark_scene(unsigned triangles) noexcept;
    struct SnapshotStamp
    {
        std::uint64_t revision{}, time_us{};
    };
    SceneSnapshot benchmark_snapshot(float phase, SnapshotStamp stamp) noexcept;
}
