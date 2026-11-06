#pragma once

#include <array>
#include <cstdint>

namespace motion::operating
{
    // Qualification candidate: physical transmission/limits still require
    // calibration. A changed microstep setting requires renewed referencing.
    inline constexpr std::uint32_t STEP_RATE = 6'250;
    inline constexpr std::array<unsigned, 3> MICROSTEPS{16, 16, 8};
    inline constexpr std::uint32_t CHECKPOINT_TICKS = 200'000;
    inline constexpr std::uint32_t COMMIT_TICKS = 400'000;
    inline constexpr std::uint32_t LEASE_TICKS = 400'000;
    inline constexpr double MAX_PROGRAM_SECONDS = 90;
    inline constexpr std::uint32_t OWNER_LEASE_MS = 250;
    inline constexpr std::uint32_t OWNER_RENEW_MS = 50;
}
