#pragma once

#include "hal/drivers/itf/IPwmOutput.hpp"

#include <cstdint>
#include <system_error>

namespace hal::util
{
    struct PwmTiming
    {
        std::uint32_t prescaler{};
        std::uint32_t period_ticks{};
        std::uint32_t high_ticks{};
        IPwmOutput::Timing actual{};
    };

    // Shared by STM32 and Linux so rounding and rejected configurations agree.
    [[nodiscard]] inline auto pwmTiming(IPwmOutput::Timing requested, std::uint32_t input_hz) noexcept
      -> util::Result<PwmTiming>
    {
        constexpr std::uint64_t ns_per_second{ 1'000'000'000ULL };
        constexpr std::uint64_t max_ticks{ 65'535U }; // CCR must also represent 100% duty.
        constexpr std::uint64_t max_divider{ 65'536U };
        if (input_hz == 0U || requested.period.count() <= 0 || requested.high_time.count() < 0 ||
            requested.high_time > requested.period) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        const auto period_ns{ static_cast<std::uint64_t>(requested.period.count()) };
        // Bound before multiplication; all following intermediate products fit uint64_t.
        const auto max_ns{ max_ticks * max_divider * ns_per_second / input_hz };
        if (period_ns > max_ns) {
            return std::unexpected(std::make_error_code(std::errc::result_out_of_range));
        }
        const auto ceil_div = [](std::uint64_t a, std::uint64_t b) { return (a + b - 1U) / b; };
        const auto cycles{ ceil_div(period_ns * input_hz, ns_per_second) };
        const auto divider{ ceil_div(cycles, max_ticks) };
        const auto period_ticks{ ceil_div(cycles, divider) };
        const auto high_cycles{ ceil_div(static_cast<std::uint64_t>(requested.high_time.count()) * input_hz,
                                         ns_per_second) };
        const auto high_ticks{ ceil_div(high_cycles, divider) };
        if (requested.high_time < requested.period && high_ticks == period_ticks) {
            return std::unexpected(std::make_error_code(std::errc::result_out_of_range));
        }
        return PwmTiming{
          .prescaler = static_cast<std::uint32_t>(divider - 1U),
          .period_ticks = static_cast<std::uint32_t>(period_ticks),
          .high_ticks = static_cast<std::uint32_t>(high_ticks),
          .actual = {
            .period = std::chrono::nanoseconds{ ceil_div(period_ticks * divider * ns_per_second, input_hz) },
            .high_time = std::chrono::nanoseconds{ ceil_div(high_ticks * divider * ns_per_second, input_hz) },
          },
        };
    }
}
