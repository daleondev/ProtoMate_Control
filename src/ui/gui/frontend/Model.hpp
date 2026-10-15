#pragma once
#include <cstdint>
namespace ui::gui
{
    // Presentation state only. Future backend messages enter here as values;
    // services/ICC never touch widgets and are never awaited by a presenter.
    struct Model
    {
        bool animating{ true };
        bool details{};
        unsigned value{ 42 };
        std::uint32_t ticks{}, interactions{};
        void tick() noexcept
        {
            if (animating) {
                ++ticks;
            }
        }
    };
}
