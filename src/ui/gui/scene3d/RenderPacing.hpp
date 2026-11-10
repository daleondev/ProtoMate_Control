#pragma once
#include <algorithm>
#include <chrono>

namespace ui::gui::scene3d
{
    // Identical on Linux and GIGA. Consume the newest pending pose only when
    // starting work, and leave at least 20% of the render cycle for other work.
    // Wall duration includes preemption: this is a conservative pacing policy,
    // not a measurement of CPU utilization.
    inline constexpr unsigned LIVE_MAX_FPS{ 5 };
    inline auto next_render(std::chrono::steady_clock::time_point start,
                            std::chrono::steady_clock::time_point end,
                            unsigned maximum_fps)
    {
        const auto interval{ std::chrono::microseconds{ 1000000 / maximum_fps } };
        const auto rest{ std::max(std::chrono::duration_cast<std::chrono::microseconds>((end - start) / 4),
                                  std::chrono::microseconds{ 1000 }) };
        return std::max(start + interval, end + rest);
    }
}
