#include "backend.hpp"

#include <chrono>
#include <thread>

#include <cstdint>
#include <limits>

#if !defined(THREADX_STD_ENABLED)
#error "The standard library must use the ThreadX gthread port"
#endif

namespace std::this_thread
{
    // This is the libstdc++ ABI entry point declared by <thread>.
    // NOLINTNEXTLINE(bugprone-reserved-identifier)
    void __sleep_for(chrono::seconds seconds, chrono::nanoseconds nanoseconds)
    {
        if (seconds.count() < 0 || nanoseconds.count() < 0) {
            return;
        }
        constexpr std::uint64_t nanoseconds_per_second{ 1'000'000'000ULL };
        const auto seconds_count{ static_cast<std::uint64_t>(seconds.count()) };
        if (seconds_count > (std::numeric_limits<std::uint64_t>::max() / nanoseconds_per_second)) {
            runtime::detail::sleep_for(std::numeric_limits<std::uint64_t>::max());
            return;
        }
        runtime::detail::sleep_for((seconds_count * nanoseconds_per_second) +
                                   static_cast<std::uint64_t>(nanoseconds.count()));
    }
}
