#include "hal/hal.hpp"

#include <exception>

namespace
{
    [[noreturn]] auto runtime_terminate_handler() noexcept -> void
    {
        const auto exception{ std::current_exception() };
        if (!exception) {
            hal::panic("Runtime terminated without an active exception");
        }

#if defined(__cpp_lib_exception_ptr_cast) && __cpp_lib_exception_ptr_cast >= 202506L
        if (const auto* error{ std::exception_ptr_cast<std::exception>(exception) }) {
            hal::panic("Unhandled C++ exception", error->what());
        }
        hal::panic("Unhandled non-std C++ exception");
#else
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& error) {
            hal::panic("Unhandled C++ exception", error.what());
        } catch (...) {
            hal::panic("Unhandled non-std C++ exception");
        }
#endif
    }
}

extern "C" auto runtime_install_terminate_handler() noexcept -> void
{
    std::set_terminate(runtime_terminate_handler);
}

namespace
{
    using PreinitFunction = void (*)() noexcept;

    // The ELF preinit array runs before ordinary C++ global constructors.
    // Both the Linux loader and STM32's __libc_init_array process this section.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    [[gnu::used, gnu::section(".preinit_array")]] PreinitFunction install_terminate_handler{
        runtime_install_terminate_handler
    };
}
