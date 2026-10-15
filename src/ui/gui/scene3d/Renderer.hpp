#pragma once
#include "Scene.hpp"
#include <span>
namespace ui::gui::scene3d
{
    struct RenderTimings
    {
        std::uint32_t clearUs{}, geometryUs{};
    };
    // No hardware, TouchGFX, thread or allocation dependency.
    bool render(const Frame& frame,
                std::span<std::uint16_t> color,
                std::span<std::uint16_t> depth,
                RenderTimings* timings = nullptr) noexcept;
}
