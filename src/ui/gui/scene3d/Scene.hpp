#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace ui::gui::scene3d
{
    inline constexpr unsigned WIDTH = 640, HEIGHT = 400, MAX_NODES = 32;
    inline constexpr std::size_t PIXELS = WIDTH * HEIGHT;
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
    struct Material
    {
        std::uint32_t color{ 0x54dbc3 };
        float ambient{ .25f }, diffuse{ .7f };
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
        bool valid() const noexcept;
    };
    struct Camera
    {
        Vec3 target{};
        float yaw{ .55f }, pitch{ .45f }, distance{ 7.5f }; // radians; right-handed Z-up
        float fieldOfView{ 40 }, nearPlane{ .1f }, farPlane{ 50 };
    };
    struct SceneSnapshot
    {
        std::array<Transform, MAX_NODES> transforms{};
        Camera camera{};
        std::uint64_t revision{}, sourceTimeUs{};
        bool valid(const Scene& scene) const noexcept;
    };
    struct Frame
    {
        const Scene* scene{};
        SceneSnapshot snapshot{};
    };
    Quaternion rotation(Vec3 axis, float radians) noexcept;
    const Scene& benchmarkScene(unsigned triangles) noexcept;
    SceneSnapshot benchmarkSnapshot(float phase, std::uint64_t revision, std::uint64_t timeUs) noexcept;
}
