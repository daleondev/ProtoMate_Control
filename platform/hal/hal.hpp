#pragma once

#if defined(HAL_PLATFORM_STM32)
#include "stm32/hal.hpp"
#elif defined(HAL_PLATFORM_LINUX)
#include "linux/hal.hpp"
#endif

#include "hal/panic.h"

#ifdef __cplusplus

#include "hal/util/Result.hpp"
#include <source_location>

namespace hal
{
    auto initialize() noexcept -> void;

    // Called once after this core's runtime services have initialized. Keep
    // secondary cores parked until potentially slow storage recovery is done.
    auto start_secondary_cores() noexcept -> void;

    // Terminal whole-device reset; inhibits motor outputs before resetting both
    // CPUs. Returns an error only on platforms without a hardware reset (Linux).
    // This is an immediate abort, not a controlled deceleration or per-core reset.
    auto reset_system() noexcept -> util::Result<>;

    [[noreturn]] auto panic(const char* message,
                            std::source_location location = std::source_location::current()) noexcept -> void;

    [[noreturn]] auto panic(const char* message,
                            const char* detail,
                            std::source_location location = std::source_location::current()) noexcept -> void;
}

#endif
