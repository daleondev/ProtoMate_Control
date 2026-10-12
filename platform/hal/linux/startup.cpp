#include "hal/hal.hpp"

namespace hal
{
    auto initialize() noexcept -> void {}

    auto panic(const char* message, std::source_location location) noexcept -> void
    {
        panic(message, nullptr, location);
    }

    auto panic(const char* message, const char* detail, std::source_location location) noexcept -> void
    {
        const HalPanicInfo info{
            .message = message,
            .detail = detail,
            .file = location.file_name(),
            .function = location.function_name(),
            .line = location.line(),
        };
        hal_panic_handler(&info);
    }
}

extern "C" [[noreturn]] void hal_error_handler() noexcept
{
    const HalPanicInfo info{
        .message = "HAL Error_Handler invoked",
        .detail = nullptr,
        .file = nullptr,
        .function = nullptr,
        .line = 0U,
    };
    hal_panic_handler(&info);
}

extern "C" [[noreturn]] void hal_fault_handler(const char* fault) noexcept
{
    const HalPanicInfo info{
        .message = fault,
        .detail = nullptr,
        .file = nullptr,
        .function = nullptr,
        .line = 0U,
    };
    hal_panic_handler(&info);
}

extern "C" [[noreturn]] void Error_Handler() { hal_error_handler(); }

