#include "backend.hpp"

#include <chrono>
#include <type_traits>

#if !defined(THREADX_STD_ENABLED)
#error "The standard library must use the ThreadX gthread port"
#endif

static_assert(std::is_same_v<std::chrono::high_resolution_clock, std::chrono::system_clock>);

namespace std::chrono
{
    steady_clock::time_point steady_clock::now() noexcept
    {
        return time_point{ duration{ runtime::detail::steady_time_nanoseconds() } };
    }

    system_clock::time_point system_clock::now() noexcept
    {
        // GCC aliases high_resolution_clock to system_clock on both supported
        // toolchains, so this definition implements both clocks.
        return time_point{ duration{ runtime::detail::system_time_nanoseconds() } };
    }
}
