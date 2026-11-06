#pragma once
#include <cstdint>

namespace io::motion
{
    // Construction failures are distinct from a missed execution deadline.
    enum class ScheduleFailure : std::uint8_t
    {
        None,
        InvalidSource,
        InvalidCut,
        FractionalBoundary,
        BrakeState,
        BrakeDuration,
        BrakeCrossing,
        BrakeOutsidePath,
        BrakeScaling,
        BankInUse
    };
}
