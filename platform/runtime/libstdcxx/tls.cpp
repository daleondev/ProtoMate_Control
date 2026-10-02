#if defined(HAL_PLATFORM_STM32)

#include "hal/hal.hpp"
#include "hal/stm32/InterruptGuard.hpp"

#include <tx_api.h>

#include <cxxabi.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

extern "C" {
extern std::byte __tdata_source[];
extern std::byte __tls_base[];
extern std::byte __tdata_source_size;
extern std::byte __tls_size;
extern std::byte __tls_align;
extern std::byte __arm32_tls_tcb_offset;
}

namespace
{
    struct ThreadLocalDestructor
    {
        void (*function)(void*);
        void* object;
        ThreadLocalDestructor* next;
    };

    // GCC's Arm bare-metal configuration uses the emutls ABI in GCC 16. This
    // ThreadX storage adaptation follows GCC's libgcc/emutls.c (FSF copyright
    // 2006-2026, GPLv3+ with GCC Runtime Library Exception 3.1); the license
    // texts are retained under vendor/gcc-16/licenses. Keep this ABI layout
    // synchronized with that source.
    struct EmutlsObject
    {
        std::uintptr_t size;
        std::uintptr_t alignment;
        union
        {
            std::uintptr_t offset;
            void* pointer;
        } location;
        const void* initializer;
    };

    struct EmutlsArray
    {
        std::size_t capacity;
        void* entries[1];
    };

    static_assert(sizeof(EmutlsObject) == 4U * sizeof(std::uintptr_t));

    ThreadLocalDestructor* startup_destructors{};
    void* startup_emutls{};
    std::uintptr_t emutls_object_count{};

    [[nodiscard]] auto linker_value(const std::byte& symbol) noexcept -> std::size_t
    {
        return reinterpret_cast<std::uintptr_t>(&symbol);
    }

    [[noreturn]] auto tls_failure() noexcept -> void
    {
        Error_Handler();
        std::abort();
    }

    [[nodiscard]] auto tls_data(std::byte* thread_pointer_value) noexcept -> std::byte*
    {
        return thread_pointer_value + linker_value(__arm32_tls_tcb_offset);
    }

    auto initialize_tls_data(std::byte* destination, const std::byte* source) noexcept -> void
    {
        const std::size_t initialized_size{ linker_value(__tdata_source_size) };
        const std::size_t total_size{ linker_value(__tls_size) };
        if (initialized_size > total_size) {
            tls_failure();
        }

        std::memcpy(destination, source, initialized_size);
        std::memset(destination + initialized_size, 0, total_size - initialized_size);
    }

    auto discard_destructors(ThreadLocalDestructor*& first) noexcept -> void
    {
        while (first != nullptr) {
            ThreadLocalDestructor* const current{ first };
            first = current->next;
            delete current;
        }
    }

    [[nodiscard]] auto emutls_array_size(std::size_t capacity) noexcept -> std::size_t
    {
        if (capacity == 0U ||
            capacity - 1U >
              (std::numeric_limits<std::size_t>::max() - sizeof(EmutlsArray)) / sizeof(void*)) {
            tls_failure();
        }
        return sizeof(EmutlsArray) + (capacity - 1U) * sizeof(void*);
    }

    [[nodiscard]] auto allocate_emutls_array(std::size_t capacity) noexcept -> EmutlsArray*
    {
        auto* const array{ static_cast<EmutlsArray*>(std::calloc(1U, emutls_array_size(capacity))) };
        if (array == nullptr) {
            tls_failure();
        }
        array->capacity = capacity;
        return array;
    }

    [[nodiscard]] auto grow_emutls_array(EmutlsArray* array, std::size_t offset) noexcept -> EmutlsArray*
    {
        const std::size_t old_capacity{ array->capacity };
        std::size_t new_capacity{
            old_capacity <= std::numeric_limits<std::size_t>::max() / 2U ? old_capacity * 2U : offset
        };
        if (new_capacity < offset) {
            if (offset > std::numeric_limits<std::size_t>::max() - 32U) {
                tls_failure();
            }
            new_capacity = offset + 32U;
        }

        auto* const grown{ static_cast<EmutlsArray*>(
          std::realloc(array, emutls_array_size(new_capacity))) };
        if (grown == nullptr) {
            tls_failure();
        }
        std::memset(grown->entries + old_capacity,
                    0,
                    (new_capacity - old_capacity) * sizeof(void*));
        grown->capacity = new_capacity;
        return grown;
    }

    [[nodiscard]] auto allocate_emutls_object(const EmutlsObject& object) noexcept -> void*
    {
        const std::size_t alignment{ static_cast<std::size_t>(object.alignment) };
        const std::size_t size{ static_cast<std::size_t>(object.size) };
        if (alignment == 0U || (alignment & (alignment - 1U)) != 0U ||
            alignment - 1U > std::numeric_limits<std::size_t>::max() - sizeof(void*) ||
            size > std::numeric_limits<std::size_t>::max() - sizeof(void*) - (alignment - 1U)) {
            tls_failure();
        }

        void* const allocation{ std::malloc(size + sizeof(void*) + alignment - 1U) };
        if (allocation == nullptr) {
            tls_failure();
        }
        const auto unaligned{ reinterpret_cast<std::uintptr_t>(allocation) + sizeof(void*) };
        const auto aligned{ (unaligned + alignment - 1U) & ~(static_cast<std::uintptr_t>(alignment) - 1U) };
        auto* const result{ reinterpret_cast<void*>(aligned) };
        static_cast<void**>(result)[-1] = allocation;

        if (object.initializer != nullptr) {
            std::memcpy(result, object.initializer, size);
        }
        else {
            std::memset(result, 0, size);
        }
        return result;
    }

    auto destroy_emutls(void*& storage) noexcept -> void
    {
        auto* const array{ static_cast<EmutlsArray*>(storage) };
        if (array == nullptr) {
            return;
        }
        for (std::size_t index{}; index < array->capacity; ++index) {
            if (array->entries[index] != nullptr) {
                std::free(static_cast<void**>(array->entries[index])[-1]);
            }
        }
        std::free(array);
        storage = nullptr;
    }

    [[nodiscard]] auto current_emutls_storage() noexcept -> void**
    {
        if (TX_THREAD* const current{ tx_thread_identify() }; current != TX_NULL) {
            return &current->tx_thread_runtime_emutls;
        }
        return &startup_emutls;
    }

    [[nodiscard]] auto emutls_offset(EmutlsObject& object) noexcept -> std::uintptr_t
    {
        std::uintptr_t offset{ __atomic_load_n(&object.location.offset, __ATOMIC_ACQUIRE) };
        if (offset == 0U) {
            const hal::stm32::InterruptGuard guard;
            offset = object.location.offset;
            if (offset == 0U) {
                if (emutls_object_count == std::numeric_limits<std::uintptr_t>::max()) {
                    tls_failure();
                }
                offset = ++emutls_object_count;
                __atomic_store_n(&object.location.offset, offset, __ATOMIC_RELEASE);
            }
        }
        return offset;
    }

    alignas(void*) thread_local std::byte exception_globals_storage[3U * sizeof(void*)]{};
    static_assert(sizeof(exception_globals_storage) == 12U);
}

extern "C" [[gnu::noinline]] auto runtime_tls_read_thread_pointer() noexcept -> void*
{
    if (TX_THREAD* const current{ tx_thread_identify() }; current != TX_NULL) {
        if (current->tx_thread_runtime_tls_block == nullptr) {
            tls_failure();
        }
        return current->tx_thread_runtime_tls_block;
    }

    const auto startup_data{ reinterpret_cast<std::uintptr_t>(__tls_base) };
    return reinterpret_cast<void*>(startup_data - linker_value(__arm32_tls_tcb_offset));
}

// __aeabi_read_tp uses the special Arm EABI thread-pointer helper convention:
// callers may keep a TLS relocation offset live in r1-r3 across the call. A
// normal C/C++ implementation is allowed to clobber those registers, so keep
// the policy in a regular helper and provide the ABI entry point as a naked
// wrapper that preserves r1-r3 and lr. r0 carries the returned thread pointer.
extern "C" [[gnu::naked]] auto __aeabi_read_tp() noexcept -> void*
{
    __asm volatile("push {r1, r2, r3, lr}\n"
                   "bl runtime_tls_read_thread_pointer\n"
                   "pop {r1, r2, r3, pc}\n");
}

extern "C" auto runtime_tls_thread_create(TX_THREAD* thread) noexcept -> void
{
    if (thread == TX_NULL) {
        tls_failure();
    }

    const std::size_t alignment{ linker_value(__tls_align) };
    const std::size_t tcb_offset{ linker_value(__arm32_tls_tcb_offset) };
    const std::size_t tls_size{ linker_value(__tls_size) };
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U || tcb_offset < alignment ||
        tls_size > std::numeric_limits<std::size_t>::max() - tcb_offset - (alignment - 1U)) {
        tls_failure();
    }

    const std::size_t allocation_size{ tcb_offset + tls_size + alignment - 1U };
    void* const allocation{ std::malloc(allocation_size) };
    if (allocation == nullptr) {
        tls_failure();
    }

    const auto allocation_address{ reinterpret_cast<std::uintptr_t>(allocation) };
    const auto aligned_address{ (allocation_address + alignment - 1U) & ~(alignment - 1U) };
    auto* const pointer{ reinterpret_cast<std::byte*>(aligned_address) };

    thread->tx_thread_runtime_tls_allocation = allocation;
    thread->tx_thread_runtime_tls_block = pointer;
    thread->tx_thread_runtime_tls_destructors = nullptr;
    thread->tx_thread_runtime_emutls = nullptr;
    initialize_tls_data(tls_data(pointer), __tdata_source);
}

extern "C" auto runtime_tls_adopt_startup(TX_THREAD* thread) noexcept -> void
{
    if (thread == TX_NULL || thread->tx_thread_runtime_tls_block == nullptr) {
        tls_failure();
    }

    // Global constructors execute in the initial execution context and may
    // already have initialized non-trivial thread_local objects, published
    // their addresses, and registered destructors that point into the linker
    // TLS image. Relocating those live objects with memcpy would violate their
    // identity and leave the destructor registrations pointing at stale data.
    // Make the application ThreadX thread continue using the original image.
    std::free(thread->tx_thread_runtime_tls_allocation);
    const auto startup_data{ reinterpret_cast<std::uintptr_t>(__tls_base) };
    thread->tx_thread_runtime_tls_allocation = nullptr;
    thread->tx_thread_runtime_tls_block =
      reinterpret_cast<void*>(startup_data - linker_value(__arm32_tls_tcb_offset));
    thread->tx_thread_runtime_tls_destructors = startup_destructors;
    startup_destructors = nullptr;
    destroy_emutls(thread->tx_thread_runtime_emutls);
    thread->tx_thread_runtime_emutls = startup_emutls;
    startup_emutls = nullptr;
}

extern "C" auto runtime_tls_thread_exit(TX_THREAD* thread) noexcept -> void
{
    if (thread == TX_NULL) {
        tls_failure();
    }

    auto*& destructor_pointer{ thread->tx_thread_runtime_tls_destructors };
    auto* current{ static_cast<ThreadLocalDestructor*>(destructor_pointer) };
    while (current != nullptr) {
        destructor_pointer = current->next;
        current->function(current->object);
        delete current;
        current = static_cast<ThreadLocalDestructor*>(destructor_pointer);
    }
}

extern "C" auto runtime_tls_thread_delete(TX_THREAD* thread) noexcept -> void
{
    if (thread == TX_NULL) {
        tls_failure();
    }

    auto* destructors{ static_cast<ThreadLocalDestructor*>(thread->tx_thread_runtime_tls_destructors) };
    discard_destructors(destructors);
    thread->tx_thread_runtime_tls_destructors = nullptr;
    std::free(thread->tx_thread_runtime_tls_allocation);
    thread->tx_thread_runtime_tls_allocation = nullptr;
    thread->tx_thread_runtime_tls_block = nullptr;
    destroy_emutls(thread->tx_thread_runtime_emutls);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wbuiltin-declaration-mismatch"
extern "C" auto __emutls_get_address(void* object_pointer) noexcept -> void*
{
    if (object_pointer == nullptr) {
        tls_failure();
    }
    auto& object{ *static_cast<EmutlsObject*>(object_pointer) };
    const std::uintptr_t offset{ emutls_offset(object) };
    void** const storage{ current_emutls_storage() };
    auto* array{ static_cast<EmutlsArray*>(*storage) };
    if (array == nullptr) {
        if (offset > std::numeric_limits<std::size_t>::max() - 32U) {
            tls_failure();
        }
        array = allocate_emutls_array(static_cast<std::size_t>(offset) + 32U);
        *storage = array;
    }
    else if (offset > array->capacity) {
        array = grow_emutls_array(array, static_cast<std::size_t>(offset));
        *storage = array;
    }

    void*& result{ array->entries[offset - 1U] };
    if (result == nullptr) {
        result = allocate_emutls_object(object);
    }
    return result;
}

extern "C" auto __emutls_register_common(void* object_pointer,
                                           std::uintptr_t size,
                                           std::uintptr_t alignment,
                                           void* initializer) noexcept -> void
{
    if (object_pointer == nullptr) {
        tls_failure();
    }
    const hal::stm32::InterruptGuard guard;
    auto& object{ *static_cast<EmutlsObject*>(object_pointer) };
    if (object.size < size) {
        object.size = size;
        object.initializer = nullptr;
    }
    if (object.alignment < alignment) {
        object.alignment = alignment;
    }
    if (initializer != nullptr && size == object.size) {
        object.initializer = initializer;
    }
}
#pragma GCC diagnostic pop

namespace __cxxabiv1
{
    extern "C" auto __cxa_thread_atexit(void (*destructor)(void*), void* object, void* dso_handle) noexcept
      -> int
    {
        static_cast<void>(dso_handle);
        auto* const element{ new (std::nothrow) ThreadLocalDestructor{
          .function = destructor, .object = object, .next = nullptr } };
        if (element == nullptr || destructor == nullptr) {
            delete element;
            return -1;
        }

        if (TX_THREAD* const current{ tx_thread_identify() }; current != TX_NULL) {
            element->next = static_cast<ThreadLocalDestructor*>(current->tx_thread_runtime_tls_destructors);
            current->tx_thread_runtime_tls_destructors = element;
        }
        else {
            element->next = startup_destructors;
            startup_destructors = element;
        }
        return 0;
    }

    extern "C" auto __cxa_get_globals() noexcept -> __cxa_eh_globals*
    {
        return reinterpret_cast<__cxa_eh_globals*>(exception_globals_storage);
    }

    extern "C" auto __cxa_get_globals_fast() noexcept -> __cxa_eh_globals*
    {
        return reinterpret_cast<__cxa_eh_globals*>(exception_globals_storage);
    }
}

#endif
