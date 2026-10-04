#include "Support.hpp"

#include <algorithm>
#include <exception>
#include <tx_thread.h>

namespace runtime::threadx
{
    bool interrupt_context() noexcept
    {
#if defined(__arm__) || defined(__thumb__)
        std::uint32_t ipsr{};
        asm volatile("mrs %0, ipsr" : "=r"(ipsr));
        return ipsr != 0U;
#else
        return false;
#endif
    }

    bool in_thread_context() noexcept
    {
        return !interrupt_context() && _tx_thread_system_state == 0U && tx_thread_identify() != TX_NULL;
    }

    ULONG duration_to_ticks(std::uint64_t nanoseconds) noexcept
    {
        constexpr ULONG maximum{ TX_WAIT_FOREVER - 1UL };
        constexpr std::uint64_t second{ 1'000'000'000ULL };
        constexpr std::uint64_t rate{ TX_TIMER_TICKS_PER_SECOND };
        const auto whole{ nanoseconds / second };
        if (whole > maximum / rate)
            return maximum;
        const auto ticks{ whole * rate + ((nanoseconds % second) * rate + second - 1U) / second };
        return static_cast<ULONG>(std::min<std::uint64_t>(ticks, maximum));
    }

    [[noreturn]] void fatal_error(const char* operation, UINT status) noexcept
    {
        static_cast<void>(operation);
        static_cast<void>(status);
        std::terminate();
    }
}
