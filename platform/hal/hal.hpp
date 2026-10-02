#pragma once

#if defined(HAL_PLATFORM_STM32)
#include "stm32/hal.hpp"
#elif defined(HAL_PLATFORM_LINUX)
#include "linux/hal.hpp"
#endif

#include "hal/panic.h"

#ifdef __cplusplus

#include <source_location>

namespace hal
{
    auto initialize() noexcept -> void;

    [[noreturn]] auto panic(const char* message,
                            std::source_location location = std::source_location::current()) noexcept -> void;

    [[noreturn]] auto panic(const char* message,
                            const char* detail,
                            std::source_location location = std::source_location::current()) noexcept -> void;
}

#endif
