#pragma once
#include <cstdint>

namespace motion_fixture
{
    struct Position
    {
        std::int64_t physical{};
        std::uint64_t accounted{};
        void observe(std::uint64_t pulses, bool forward) noexcept
        {
            // HAL clear/start can reset the per-sequence count in Idle as well
            // as Running. A reset is not motion in the newly selected direction.
            if (pulses < accounted) accounted = 0;
            const auto delta = std::int64_t(pulses - accounted);
            accounted = pulses;
            physical += forward ? delta : -delta;
        }
    };
}
