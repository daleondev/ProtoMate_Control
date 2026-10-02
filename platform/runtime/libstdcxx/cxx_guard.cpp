#include "backend.hpp"
#include "hal/hal.hpp"

#include <cstddef>
#include <cstdint>

// Both supported targets use the Arm EABI/Itanium guard-byte convention: byte
// zero records completion and byte one records an initializer in progress.
// The guard object itself is four bytes on Arm and eight bytes on Linux, so the
// ABI entry points deliberately accept an opaque pointer.
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);

// These names and mutable C objects are required by the C++ ABI and ThreadX.
// NOLINTBEGIN(bugprone-reserved-identifier,cppcoreguidelines-avoid-c-arrays,cppcoreguidelines-avoid-non-const-global-variables,modernize-avoid-c-arrays)

namespace
{
    constexpr std::size_t INITIALIZED_BYTE{ 0U };
    constexpr std::size_t IN_PROGRESS_BYTE{ 1U };

    TX_MUTEX cxx_guard_mutex;
    CHAR cxx_guard_mutex_name[] = "C++ static init";
    bool cxx_guard_mutex_ready{};

    [[nodiscard]] bool interrupt_context() noexcept
    {
#if defined(HAL_PLATFORM_STM32)
        return __get_IPSR() != 0U;
#else
        return false;
#endif
    }

    [[noreturn]] void guard_failure(const char* operation, UINT status) noexcept
    {
        runtime::detail::fatal_error(operation, status);
    }

    [[nodiscard]] bool locking_required() noexcept
    {
        if (interrupt_context()) {
            guard_failure("static initialization from interrupt", TX_CALLER_ERROR);
        }

        if (!runtime::detail::active()) {
            // Global constructors run before the scheduler and are inherently
            // single-threaded, so no ThreadX object is required yet.
            return false;
        }

        if (tx_thread_identify() == TX_NULL) {
            return false;
        }

        if (!cxx_guard_mutex_ready) {
            guard_failure("static initialization before runtime setup", TX_MUTEX_ERROR);
        }
        return true;
    }

    [[nodiscard]] std::uint8_t load_guard_byte(const void* guard, std::size_t offset) noexcept
    {
        const auto* bytes{ static_cast<const std::uint8_t*>(guard) };
        return __atomic_load_n(bytes + offset, __ATOMIC_ACQUIRE);
    }

    void store_guard_byte(void* guard, std::size_t offset, std::uint8_t value, int order) noexcept
    {
        auto* bytes{ static_cast<std::uint8_t*>(guard) };
        __atomic_store_n(bytes + offset, value, order);
    }

    void release_guard_mutex(bool locked) noexcept
    {
        if (!locked) {
            return;
        }
        const UINT status{ tx_mutex_put(&cxx_guard_mutex) };
        if (status != TX_SUCCESS) {
            guard_failure("tx_mutex_put static initialization", status);
        }
    }

    void acquire_guard_mutex(bool locked) noexcept
    {
        if (!locked) {
            return;
        }
        const UINT status{ tx_mutex_get(&cxx_guard_mutex, TX_WAIT_FOREVER) };
        if (status != TX_SUCCESS) {
            guard_failure("tx_mutex_get static initialization", status);
        }
    }
}

namespace runtime::detail
{
    UINT initialize_cxx_guard() noexcept
    {
        if (cxx_guard_mutex_ready) {
            return TX_SUCCESS;
        }

        const UINT status{ tx_mutex_create(&cxx_guard_mutex, cxx_guard_mutex_name, TX_INHERIT) };
        if (status == TX_SUCCESS) {
            cxx_guard_mutex_ready = true;
        }
        return status;
    }

    void destroy_cxx_guard() noexcept
    {
        if (!cxx_guard_mutex_ready) {
            return;
        }
        const UINT status{ tx_mutex_delete(&cxx_guard_mutex) };
        if (status != TX_SUCCESS) {
            guard_failure("tx_mutex_delete static initialization", status);
        }
        cxx_guard_mutex_ready = false;
    }
}

extern "C" int __cxa_guard_acquire(void* guard)
{
    if (guard == nullptr) {
        guard_failure("__cxa_guard_acquire", TX_PTR_ERROR);
    }

    if (load_guard_byte(guard, INITIALIZED_BYTE) != 0U) {
        return 0;
    }

    const bool locked{ locking_required() };
    while (true) {
        acquire_guard_mutex(locked);
        if (load_guard_byte(guard, INITIALIZED_BYTE) != 0U) {
            release_guard_mutex(locked);
            return 0;
        }
        if (load_guard_byte(guard, IN_PROGRESS_BYTE) == 0U) {
            store_guard_byte(guard, IN_PROGRESS_BYTE, 1U, __ATOMIC_RELAXED);
            release_guard_mutex(locked);
            return 1;
        }
        release_guard_mutex(locked);

        if (!locked) {
            guard_failure("recursive static initialization", TX_NOT_DONE);
        }
        // Only callers initializing this same object must wait. Holding the
        // global guard mutex across user code can deadlock an initializer that
        // joins a thread which initializes an unrelated static object.
        const UINT status{ tx_thread_sleep(1U) };
        if (status != TX_SUCCESS) {
            guard_failure("static initialization wait", status);
        }
    }
}

extern "C" void __cxa_guard_release(void* guard)
{
    if (guard == nullptr) {
        guard_failure("__cxa_guard_release", TX_PTR_ERROR);
    }

    const bool locked{ locking_required() };
    acquire_guard_mutex(locked);
    store_guard_byte(guard, IN_PROGRESS_BYTE, 0U, __ATOMIC_RELAXED);
    store_guard_byte(guard, INITIALIZED_BYTE, 1U, __ATOMIC_RELEASE);
    release_guard_mutex(locked);
}

extern "C" void __cxa_guard_abort(void* guard)
{
    if (guard == nullptr) {
        guard_failure("__cxa_guard_abort", TX_PTR_ERROR);
    }

    const bool locked{ locking_required() };
    acquire_guard_mutex(locked);
    store_guard_byte(guard, IN_PROGRESS_BYTE, 0U, __ATOMIC_RELEASE);
    release_guard_mutex(locked);
}

// NOLINTEND(bugprone-reserved-identifier,cppcoreguidelines-avoid-c-arrays,cppcoreguidelines-avoid-non-const-global-variables,modernize-avoid-c-arrays)
