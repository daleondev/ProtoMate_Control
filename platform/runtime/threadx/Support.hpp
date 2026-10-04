#pragma once

#include <cstdint>
#include <tx_api.h>

namespace runtime::threadx
{
    [[nodiscard]] bool interrupt_context() noexcept;
    [[nodiscard]] bool in_thread_context() noexcept;
    [[nodiscard]] ULONG duration_to_ticks(std::uint64_t nanoseconds) noexcept;
    [[noreturn]] void fatal_error(const char* operation, UINT status) noexcept;
}
