#include "backend.hpp"
#include <limits>
#include <tx_api.h>
namespace runtime::detail
{
    namespace
    {
        // Per-core tick extension, serialized by the runtime interrupt lock.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        TickExtension tick_state{};
        constexpr std::uint64_t NANOSECONDS_PER_SECOND{ 1'000'000'000ULL };
    }
    void initialize_steady_clock() noexcept
    {
        tick_state = { .previous = static_cast<std::uint32_t>(tx_time_get()) };
    }
    [[nodiscard]] std::int64_t ticks_to_nanoseconds(std::uint64_t ticks) noexcept
    {
        const std::uint64_t seconds{ ticks / TX_TIMER_TICKS_PER_SECOND };
        const std::uint64_t remainder{ ticks % TX_TIMER_TICKS_PER_SECOND };
        constexpr auto maximum{ static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) };
        const std::uint64_t fractional_nanoseconds{ remainder * NANOSECONDS_PER_SECOND /
                                                    TX_TIMER_TICKS_PER_SECOND };
        if (seconds > (maximum - fractional_nanoseconds) / NANOSECONDS_PER_SECOND) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return static_cast<std::int64_t>((seconds * NANOSECONDS_PER_SECOND) + fractional_nanoseconds);
    }
    std::uint64_t steady_ticks() noexcept
    {
        const ULONG old_posture{ tx_interrupt_control(TX_INT_DISABLE) };
        const auto current_tick{ static_cast<std::uint32_t>(tx_time_get()) };
        const std::uint64_t result{ extend_tick_counter(tick_state, current_tick) };
        static_cast<void>(tx_interrupt_control(old_posture));
        return result;
    }

    std::uint64_t extend_tick_counter(TickExtension& state, std::uint32_t current) noexcept
    {
        if (current < state.previous) {
            state.epoch += (1ULL << std::numeric_limits<std::uint32_t>::digits);
        }
        state.previous = current;
        return state.epoch | current;
    }

    std::int64_t steady_time_nanoseconds() noexcept { return ticks_to_nanoseconds(steady_ticks()); }

}
