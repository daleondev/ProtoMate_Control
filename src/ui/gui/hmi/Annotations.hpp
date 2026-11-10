#pragma once
#include "ui/gui/scene3d/Scene.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <touchgfx/hal/Types.hpp>
namespace ui::gui::hmi
{
    // Frame-local layout: never combine projected anchors with newer telemetry.
    inline std::array<touchgfx::Rect, 3> layout_badges(std::array<scene3d::ProjectedAnchor, 3>& anchors)
    {
        constexpr int width{ 136 };
        constexpr int height{ 56 };
        constexpr int left{ 8 };
        constexpr int right{ 496 };
        constexpr int top{ 56 };
        constexpr int bottom{ 300 };
        constexpr std::array sides{ -148, 24 };
        constexpr std::array offsets{ -28, 36, -92, 100, -156, 164, -220, 228, -284, 292 };
        std::array<touchgfx::Rect, 3> result{};
        for (unsigned i{ 0 }; i < anchors.size(); ++i) {
            auto& a{ anchors[i] };
            if (!a.visible) {
                continue;
            }
            int best{ std::numeric_limits<int>::max() };
            for (const int side : sides) {
                for (const int offset : offsets) {
                    touchgfx::Rect box{ int16_t(std::clamp(int(a.x) + side, left, right)),
                                        // Keep top controls and the bottom Reset View
                                        // control clear in viewport-local coordinates.
                                        int16_t(std::clamp(int(a.y) + offset, top, bottom)),
                                        width,
                                        height };
                    bool overlaps{};
                    for (unsigned j{ 0 }; j < i; ++j) {
                        overlaps |= anchors[j].visible && box.intersect(result[j]);
                    }
                    const int dx{ box.x + (width / 2) - a.x };
                    const int dy{ box.y + (height / 2) - a.y };
                    const int cost{ (dx * dx) + (dy * dy) };
                    if (!overlaps && cost < best) {
                        best = cost;
                        result[i] = box;
                    }
                }
            }
            if (best == std::numeric_limits<int>::max()) {
                a.visible = false;
            }
        }
        return result;
    }
}
