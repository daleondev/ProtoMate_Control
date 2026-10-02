#include "hal/hal.hpp"

#include <tx_api.h>
#include <tx_thread.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <exception>

// These names are prescribed by GNU ld's --wrap convention.
// NOLINTNEXTLINE(bugprone-reserved-identifier)
extern "C" int __real_main(int argc, char** argv);
extern "C" void runtime_application_define() __attribute__((weak));
extern "C" void runtime_filex_initialize();
#if defined(RUNTIME_HARDWARE_STORAGE_STARTUP_TEST)
extern "C" void hardware_storage_startup_test_select();
#endif
#if defined(HAL_PLATFORM_LINUX)
extern "C" void runtime_application_prepare() __attribute__((weak));
#endif

namespace
{
#ifndef RUNTIME_APPLICATION_THREAD_STACK_SIZE
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define RUNTIME_APPLICATION_THREAD_STACK_SIZE 4096U
#endif

    constexpr std::size_t APPLICATION_THREAD_STACK_SIZE{ RUNTIME_APPLICATION_THREAD_STACK_SIZE };
    constexpr UINT APPLICATION_THREAD_PRIORITY{ 15U };
    constexpr std::size_t STACK_ALIGNMENT{ 8U };
    constexpr std::size_t APPLICATION_THREAD_NAME_SIZE{ sizeof("Application Thread") };

    // ThreadX owns and mutates this statically allocated control block and stack.
    // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
    alignas(STACK_ALIGNMENT) std::array<std::byte, APPLICATION_THREAD_STACK_SIZE> application_thread_stack{};
    std::array<CHAR, APPLICATION_THREAD_NAME_SIZE> application_thread_name{ "Application Thread" };
    TX_THREAD application_thread;
    int application_argc{};
    char** application_argv{};
    // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

    auto assert_tx_call(UINT status) noexcept -> void
    {
        if (status != TX_SUCCESS) {
            Error_Handler();
        }
    }

    auto thread_stack_error_handler(TX_THREAD* thread) noexcept -> void
    {
        // Do not call libc or allocate on the already compromised stack.
        static_cast<void>(thread);
        Error_Handler();
    }

    auto application_thread_entry(ULONG input) noexcept -> void
    {
        static_cast<void>(input);
#if defined(HAL_PLATFORM_STM32)
#if defined(RUNTIME_HARDWARE_STORAGE_STARTUP_TEST)
        hardware_storage_startup_test_select();
#endif
        runtime_filex_initialize();
#endif
        int exit_status{};
        try {
            exit_status = __real_main(application_argc, application_argv);
        } catch (...) {
            // Do not permit user exceptions to cross the ThreadX entry frame.
            std::terminate();
        }

#if defined(HAL_PLATFORM_LINUX)
        std::exit(exit_status);
#else
        if (exit_status != EXIT_SUCCESS) {
            Error_Handler();
        }
#endif
    }
}

// NOLINTNEXTLINE(bugprone-reserved-identifier)
extern "C" int __wrap_main(int argc, char** argv)
{
#if defined(HAL_PLATFORM_LINUX)
    application_argc = argc;
    application_argv = argv;
#else
    static_cast<void>(argc);
    static_cast<void>(argv);
#endif

#if defined(HAL_PLATFORM_LINUX)
    if (runtime_application_prepare != nullptr) {
        runtime_application_prepare();
    }
#endif
    hal::initialize();
    tx_kernel_enter();
    Error_Handler();
    return EXIT_FAILURE;
}

extern "C" void tx_application_define(void* first_unused_memory)
{
    static_cast<void>(first_unused_memory);

    assert_tx_call(tx_thread_stack_error_notify(thread_stack_error_handler));

    // This must remain the first ThreadX thread created by the application.
    assert_tx_call(tx_thread_create(&application_thread,
                                    application_thread_name.data(),
                                    application_thread_entry,
                                    0U,
                                    application_thread_stack.data(),
                                    static_cast<ULONG>(application_thread_stack.size()),
                                    APPLICATION_THREAD_PRIORITY,
                                    APPLICATION_THREAD_PRIORITY,
                                    TX_NO_TIME_SLICE,
                                    TX_AUTO_START));

    // This creates the runtime's reaper thread, after the application thread.
    runtime_libstdcxx_initialize();

    if (runtime_application_define != nullptr) {
        runtime_application_define();
    }

#if defined(HAL_PLATFORM_STM32)
    // Global constructors and the application-definition hook execute before
    // ThreadX starts. Transfer their TLS only after all startup code finishes,
    // so user main continues with the complete initial execution context.
    runtime_tls_adopt_startup(&application_thread);
#endif
}
