#pragma once
#include "profiling/Trace.h"
#include <cstdint>

namespace diagnostics
{
    // Fixed representation is part of the existing protocol/diagnostic contract.
    // NOLINTNEXTLINE(performance-enum-size)
    enum class ProfileOperation : std::uint32_t
    {
        Status,
        Arm,
        Freeze
    };
    struct ProfileRequest
    {
        ProfileOperation operation{};
        std::uint32_t preset{}, capture{};
    };
    struct ProfileResponse
    {
        std::uint32_t result{};
        ProfileStatus status{};
    };
    ProfileResponse control_profile(ProfileRequest request);
    // Existing maintenance workers call this; samples at most once per second.
    void poll_profiling();
}
