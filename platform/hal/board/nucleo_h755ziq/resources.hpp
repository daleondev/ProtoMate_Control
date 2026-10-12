#pragma once

#include "hal/util/Core.hpp"
#include "hal/util/ResourceAccess.hpp"

namespace hal::board::resources
{
    // Physical assignments must agree with the per-core CubeMX configuration.
    inline constexpr gpio::Pin M4_STATUS_LED{gpio::Port::B, 0};
    inline constexpr gpio::Pin M7_STATUS_LED{gpio::Port::E, 1};
    inline constexpr auto M4_RUNTIME_TIMER{timer::Peripheral::Tim5};
    inline constexpr auto M7_RUNTIME_TIMER{timer::Peripheral::Tim12};

    [[nodiscard]] constexpr auto allows(util::Core core, gpio::Pin pin) noexcept -> bool
    {
        return (core == util::Core::M4 && pin == M4_STATUS_LED) ||
               (core == util::Core::M7 && pin == M7_STATUS_LED);
    }

    [[nodiscard]] constexpr auto allows(util::Core core, timer::Peripheral timer) noexcept -> bool
    {
        return (core == util::Core::M4 && timer == M4_RUNTIME_TIMER) ||
               (core == util::Core::M7 && timer == M7_RUNTIME_TIMER);
    }
}
