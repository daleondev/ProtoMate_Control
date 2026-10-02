#pragma once

#include <tx_api.h>

#include <cstdint>
#include <ctime>
#include <optional>
#include <string_view>

namespace runtime
{
    [[nodiscard]] UINT initialize() noexcept;

    namespace thread
    {
        struct Attributes
        {
            // Thread name. Automatically generated if empty.
            std::string_view name{ "" };

            // ThreadX priorities are ordered from 0 (highest) to
            // TX_MAX_PRIORITIES - 1 (lowest). -1 selects the configured default.
            std::int32_t priority{ -1 };

            // Stack size in bytes. Zero selects the configured default.
            std::size_t stack_size{};
        };

        // Low-level escape hatch for APIs that create a thread internally.
        // Prefer runtime::thread::create/create_jthread for direct creation.
        // The attributes apply once to the next standard thread created by the
        // calling thread; publishing again first replaces them.
        void publish_attributes(const Attributes& attributes) noexcept;

        [[nodiscard]] std::optional<Attributes> consume_attributes() noexcept;
    }

    namespace detail
    {
        struct MutexHandle
        {
            void* implementation{};
        };

        using RecursiveMutexHandle = MutexHandle;

        struct ConditionHandle
        {
            void* implementation{};
        };

        struct OnceHandle
        {
            unsigned int state{};
        };

        struct SemaphoreHandle
        {
            void* implementation{};
            unsigned int initial_count{};
        };

        using ThreadHandle = TX_THREAD*;
        using KeyHandle = unsigned int;
        using TimePoint = timespec;

        [[nodiscard]] bool active() noexcept;
        [[nodiscard]] bool in_thread_context() noexcept;

        [[nodiscard]] UINT initialize_cxx_guard() noexcept;
        void destroy_cxx_guard() noexcept;

        int thread_create(ThreadHandle* thread,
                          void* (*entry)(void*),
                          void* argument,
                          const thread::Attributes& attributes) noexcept;
        int thread_join(ThreadHandle thread, void** result) noexcept;
        int thread_detach(ThreadHandle thread) noexcept;
        [[nodiscard]] ThreadHandle thread_self() noexcept;
        int thread_yield() noexcept;

        int key_create(KeyHandle* key, void (*destructor)(void*)) noexcept;
        int key_delete(KeyHandle key) noexcept;
        [[nodiscard]] void* key_get(KeyHandle key) noexcept;
        int key_set(KeyHandle key, const void* value) noexcept;

        void mutex_init(MutexHandle* mutex) noexcept;
        void recursive_mutex_init(RecursiveMutexHandle* mutex) noexcept;
        int mutex_destroy(MutexHandle* mutex) noexcept;
        int recursive_mutex_destroy(RecursiveMutexHandle* mutex) noexcept;
        int mutex_lock(MutexHandle* mutex) noexcept;
        int recursive_mutex_lock(RecursiveMutexHandle* mutex) noexcept;
        int mutex_try_lock(MutexHandle* mutex) noexcept;
        int recursive_mutex_try_lock(RecursiveMutexHandle* mutex) noexcept;
        int mutex_timed_lock(MutexHandle* mutex, const TimePoint* deadline) noexcept;
        int recursive_mutex_timed_lock(RecursiveMutexHandle* mutex, const TimePoint* deadline) noexcept;
        int mutex_unlock(MutexHandle* mutex) noexcept;
        int recursive_mutex_unlock(RecursiveMutexHandle* mutex) noexcept;

        void condition_init(ConditionHandle* condition) noexcept;
        int condition_destroy(ConditionHandle* condition) noexcept;
        int condition_wait(ConditionHandle* condition, MutexHandle* mutex) noexcept;
        int condition_wait_recursive(ConditionHandle* condition, RecursiveMutexHandle* mutex) noexcept;
        int condition_timed_wait(ConditionHandle* condition,
                                 MutexHandle* mutex,
                                 const TimePoint* deadline) noexcept;
        int condition_signal(ConditionHandle* condition) noexcept;
        int condition_broadcast(ConditionHandle* condition) noexcept;

        int once(OnceHandle* once_control, void (*function)());

        int semaphore_init(SemaphoreHandle* semaphore, unsigned int value) noexcept;
        int semaphore_destroy(SemaphoreHandle* semaphore) noexcept;
        int semaphore_wait(SemaphoreHandle* semaphore) noexcept;
        int semaphore_try_wait(SemaphoreHandle* semaphore) noexcept;
        int semaphore_timed_wait(SemaphoreHandle* semaphore, const TimePoint* deadline) noexcept;
        int semaphore_post(SemaphoreHandle* semaphore) noexcept;

        [[nodiscard]] std::uint64_t steady_ticks() noexcept;
        [[nodiscard]] std::uint64_t extend_tick_counter(std::uint64_t& epoch,
                                                        std::uint32_t& previous,
                                                        std::uint32_t current) noexcept;
        [[nodiscard]] std::int64_t steady_time_nanoseconds() noexcept;
        [[nodiscard]] std::int64_t system_time_nanoseconds() noexcept;
        [[nodiscard]] ULONG duration_to_ticks(std::uint64_t nanoseconds) noexcept;
        void sleep_for(std::uint64_t nanoseconds) noexcept;

        [[noreturn]] void fatal_error(const char* operation, UINT status) noexcept;
    }
}
