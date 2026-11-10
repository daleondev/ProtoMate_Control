#pragma once
#include "Scene.hpp"
#include <span>
namespace ui::gui::scene3d
{
    struct RenderTimings
    {
        std::uint32_t clear_us{}, geometry_us{}, grid_us{};
        std::array<std::uint32_t, 4> node_us{};
    };
    // No hardware, TouchGFX, thread or allocation dependency.
    bool render(const Frame& frame,
                std::span<std::uint16_t> color,
                std::span<std::uint16_t> depth,
                RenderTimings* timings = nullptr,
                std::array<ProjectedAnchor, 3>* annotations = nullptr) noexcept;
}
