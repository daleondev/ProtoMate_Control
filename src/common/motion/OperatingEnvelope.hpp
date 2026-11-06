#pragma once

#include <array>
#include <cstdint>

namespace motion::operating
{
    // Qualification candidate: physical transmission/limits still require
    // calibration. A changed microstep setting requires renewed referencing.
    inline constexpr std::uint32_t STEP_RATE = 6'250;
    inline constexpr std::array<unsigned, 3> MICROSTEPS{16, 16, 8};
    // 10 MHz timeline: renew every 40 ms, installing a stop boundary 80 ms
    // ahead. The execution worker still services refills/stops every millisecond.
    inline constexpr std::uint32_t CHECKPOINT_TICKS = 400'000;
    inline constexpr std::uint32_t COMMIT_TICKS = 800'000;
    // Local worker keepalive; independent of both replacement cadence and the
    // UI ownership lease below. A slower renewal cadence does not slow polling.
    inline constexpr std::uint32_t LEASE_TICKS = 400'000;
    inline constexpr double MAX_PROGRAM_SECONDS = 90;
    inline constexpr std::uint32_t OWNER_LEASE_MS = 250;
    inline constexpr std::uint32_t OWNER_RENEW_MS = 50;
}
