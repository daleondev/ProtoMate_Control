#pragma once
#include "ui/gui/scene3d/Scene.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <touchgfx/hal/Types.hpp>
namespace ui::gui::hmi
{
    // Frame-local layout: never combine projected anchors with newer telemetry.
    inline std::array<touchgfx::Rect, 3> layoutBadges(std::array<scene3d::ProjectedAnchor, 3>& anchors)
    {
        std::array<touchgfx::Rect, 3> result{};
        for (unsigned i = 0; i < anchors.size(); ++i) {
            auto& a = anchors[i];
            if (!a.visible)
                continue;
            int best = std::numeric_limits<int>::max();
            for (const int side : { -148, 24 })
                for (const int offset : { -28, 36, -92, 100, -156, 164, -220, 228, -284, 292 }) {
                    touchgfx::Rect box{ int16_t(std::clamp(int(a.x) + side, 8, 496)),
                                        // Keep top controls and the bottom Reset View
                                        // control clear in viewport-local coordinates.
                                        int16_t(std::clamp(int(a.y) + offset, 56, 300)),
                                        136,
                                        56 };
                    bool overlaps{};
                    for (unsigned j = 0; j < i; ++j)
                        overlaps |= anchors[j].visible && box.intersect(result[j]);
                    const int dx = box.x + 68 - a.x, dy = box.y + 28 - a.y;
                    const int cost = dx * dx + dy * dy;
                    if (!overlaps && cost < best) {
                        best = cost;
                        result[i] = box;
                    }
                }
            if (best == std::numeric_limits<int>::max())
                a.visible = false;
        }
        return result;
    }
}
