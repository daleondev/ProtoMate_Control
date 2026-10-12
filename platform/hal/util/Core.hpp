#pragma once

#include <cstdint>

namespace hal::util
{
    enum class Core : std::uint8_t
    {
        M4,
        M7,
        Host
    };

    [[nodiscard]] constexpr auto executing_core() noexcept -> Core
    {
#if defined(CORE_CM4) && defined(CORE_CM7)
#error "A firmware image must select exactly one core"
#elif defined(CORE_CM4)
        return Core::M4;
#elif defined(CORE_CM7)
        return Core::M7;
#elif defined(HAL_PLATFORM_STM32)
#error "The hardware core must be selected by the build"
#else
        return Core::Host;
#endif
    }
}
