#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace communication
{
    // Shared contract for byte transports and physical inter-core channels.
    // Values index the shared-memory queues; priority is independent of index.
    enum class Lane : std::uint8_t
    {
        Normal = 0,
        Urgent = 1,
        Background = 2
    };

    inline constexpr std::array LANE_PRIORITY{ Lane::Urgent, Lane::Normal, Lane::Background };
    inline constexpr std::size_t LANE_COUNT{ LANE_PRIORITY.size() };
    inline constexpr std::size_t MAX_BACKGROUND_FRAME_BYTES{ 512 };
}
