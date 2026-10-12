#pragma once

#include "pneumo/units.hpp"

namespace io::motion
{
    using namespace pnm::units::literals;

    enum class BufferMode : std::uint8_t
    {
        Aborting,
        Buffered,
        BlendingLow,
        BlendingPrevious,
        BlendingNext,
        BlendingHigh
    };

    inline constexpr auto DEFAULT_REFERENCE_SEEK{ 5_rpm };
    inline constexpr auto DEFAULT_REFERENCE_LATCH{ 0.5_rpm };
    inline constexpr auto DEFAULT_REFERENCE_TIMEOUT{ 30_s };
    inline constexpr auto DEFAULT_GROUP_REFERENCE_TIMEOUT{ 90_s };
    inline constexpr auto DEFAULT_ACCELERATION{ 3600_deg_s2 };
    inline constexpr auto DEFAULT_JERK{ 36000_deg_s3 };
    inline constexpr auto MOTION_HISTORY_CAPACITY{ 32UZ };

    struct Defaults
    {
        pnm::units::AngularAcceleration acceleration{ DEFAULT_ACCELERATION };
        pnm::units::AngularAcceleration deceleration{ DEFAULT_ACCELERATION };
        pnm::units::AngularJerk jerk{ DEFAULT_JERK };
        BufferMode buffer_mode{ BufferMode::Aborting };
    };
}