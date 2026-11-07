#pragma once
#include "profiling/Trace.h"
#include <cstdint>

namespace diagnostics
{
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
    ProfileResponse controlProfile(ProfileRequest request);
    // Existing maintenance workers call this; samples at most once per second.
    void pollProfiling();
}
