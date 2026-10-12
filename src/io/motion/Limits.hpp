#pragma once

#include <chrono>
#include <cstdint>

namespace io::motion::limits
{
    using namespace std::chrono_literals;
    // Match the shared step generator's tick, minimum period and counter horizon.
    inline constexpr auto TIMER_TICK{ 100ns };
    inline constexpr std::chrono::nanoseconds MIN_PERIOD{ 10us };
    inline constexpr auto HIGH_TIME{ 5us };
    inline constexpr auto MAX_PERIOD{ 53'687'091'100ns };
    inline constexpr double NANOSECONDS_PER_SECOND{ 1e9 };
    inline constexpr double TICKS_PER_SECOND{ NANOSECONDS_PER_SECOND / TIMER_TICK.count() };
    // Keep one bit of precision in double pulse coordinates for inversion/rounding.
    inline constexpr std::uint64_t MAX_PULSES{ std::uint64_t{ 1 } << 52U };
    inline constexpr double MAX_PLANNED_RATE{ 99'999.0 };
    inline constexpr auto COORDINATED_START_DELAY{ 10ms };
    inline constexpr auto COORDINATED_DURATION_MARGIN{ COORDINATED_START_DELAY + HIGH_TIME };
    inline constexpr unsigned BLEND_SEARCH_STEPS{ 32U };
    inline constexpr unsigned BLEND_REFINE_STEPS{ 24U };
    inline constexpr auto BOUND_ROUNDING_MARGIN = 2 * TIMER_TICK;
    inline constexpr double NANOSECOND_ROUNDING_TOLERANCE{ 0.001 };
    inline constexpr auto MAX_BOUND = std::chrono::nanoseconds{ 1'000'000'000'000'000 };
    inline constexpr double MAX_PROFILE_SECONDS{ 1e8 };
    inline constexpr double SYNCHRONIZED_HORIZON_SECONDS{ 53.68 };
    inline constexpr double NORMALIZATION_TOLERANCE{ 1e-8 };
    inline constexpr double RATE_TOLERANCE{ 1e-9 };
    inline constexpr double DYNAMICS_TOLERANCE{ 1e-7 };
}
