#include "backend.hpp"
#include "hal/drivers/factory/rtc.hpp"
#include "hal/drivers/factory/timer.hpp"
#include "hal/hal.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <utility>

// ThreadX requires mutable C control blocks, raw stack storage, C callbacks,
// and explicit lifetime management at this implementation boundary.
// NOLINTBEGIN(bugprone-easily-swappable-parameters,cppcoreguidelines-avoid-c-arrays,cppcoreguidelines-avoid-magic-numbers,cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-macro-usage,cppcoreguidelines-missing-std-forward,cppcoreguidelines-owning-memory,cppcoreguidelines-pro-bounds-array-to-pointer-decay,cppcoreguidelines-pro-type-const-cast,cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-type-vararg,modernize-avoid-c-arrays,readability-magic-numbers,readability-math-missing-parentheses,readability-named-parameter)

#ifndef RUNTIME_STD_THREAD_STACK_SIZE
#define RUNTIME_STD_THREAD_STACK_SIZE 4096U
#endif

#ifndef RUNTIME_STD_THREAD_PRIORITY
#define RUNTIME_STD_THREAD_PRIORITY 16U
#endif

namespace
{
    thread_local std::optional<runtime::thread::Attributes> pending_attributes;

    void cleanup_thread_runtime(TX_THREAD* thread) noexcept;
}

namespace runtime
{
    namespace thread
    {
        void publish_attributes(const Attributes& attributes) noexcept { pending_attributes = attributes; }

        std::optional<Attributes> consume_attributes() noexcept
        {
            auto result{ pending_attributes };
            pending_attributes.reset();
            return result;
        }
    }

    namespace detail
    {
        namespace
        {
            constexpr std::size_t STACK_ALIGNMENT{ 8U };
            constexpr std::size_t REAPER_STACK_SIZE{ 2048U };
            constexpr UINT REAPER_PRIORITY{ 31U };
            constexpr std::size_t CLEANUP_QUEUE_DEPTH{ 16U };
            constexpr ULONG ROLLOVER_SAMPLE_TICKS{ 0x7FFFFFFFUL };
            constexpr ULONG MAX_FINITE_WAIT{ TX_WAIT_FOREVER - 1UL };
            constexpr std::uint64_t NANOSECONDS_PER_SECOND{ 1'000'000'000ULL };
            constexpr std::size_t HIGH_RESOLUTION_TIMER_INDEX{ 5U };

            struct HighResolutionCounter
            {
                std::uint64_t ticks{};
                std::uint64_t ticks_per_second{};
                std::uint64_t modulus{};
                std::uint32_t state_version{};
            };

            [[nodiscard]] std::int64_t ticks_to_nanoseconds(std::uint64_t ticks) noexcept
            {
                const std::uint64_t seconds{ ticks / TX_TIMER_TICKS_PER_SECOND };
                const std::uint64_t remainder{ ticks % TX_TIMER_TICKS_PER_SECOND };
                constexpr auto maximum{ static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max()) };
                const std::uint64_t fractional_nanoseconds{ remainder * NANOSECONDS_PER_SECOND /
                                                            TX_TIMER_TICKS_PER_SECOND };
                if (seconds > (maximum - fractional_nanoseconds) / NANOSECONDS_PER_SECOND) {
                    return std::numeric_limits<std::int64_t>::max();
                }
                return static_cast<std::int64_t>(seconds * NANOSECONDS_PER_SECOND + fractional_nanoseconds);
            }

            struct MutexImplementation
            {
                TX_MUTEX mutex{};
            };

            struct Waiter
            {
                Waiter* previous{};
                Waiter* next{};
                TX_SEMAPHORE semaphore{};
                bool linked{};
            };

            struct ConditionImplementation
            {
                TX_MUTEX mutex{};
                Waiter* first{};
                Waiter* last{};
            };

            struct SemaphoreImplementation
            {
                TX_SEMAPHORE semaphore{};
            };

            struct KeyDefinition
            {
                void (*destructor)(void*){};
                unsigned int generation{};
                bool allocated{};
            };

            struct ThreadControl
            {
                TX_THREAD thread{};
                ThreadControl* registry_next{};
                ULONG registry_id{};
                void* stack{};
                void* (*entry)(void*){};
                void* argument{};
                TX_MUTEX lifecycle_mutex{};
                TX_SEMAPHORE completion{};
                TX_SEMAPHORE* cleanup_ack{};
                bool detached{};
                bool finished{};
                bool cleanup_queued{};
                char name[32]{};
            };

            static_assert(offsetof(ThreadControl, thread) == 0U);
            static_assert((STACK_ALIGNMENT & (STACK_ALIGNMENT - 1U)) == 0U);
            static_assert(alignof(std::max_align_t) >= STACK_ALIGNMENT);
            static_assert(RUNTIME_STD_THREAD_STACK_SIZE >= TX_MINIMUM_STACK);
            static_assert(RUNTIME_STD_THREAD_STACK_SIZE % STACK_ALIGNMENT == 0U);
            static_assert(RUNTIME_STD_THREAD_STACK_SIZE <= std::numeric_limits<ULONG>::max());
            static_assert(RUNTIME_STD_THREAD_PRIORITY < TX_MAX_PRIORITIES);
            static_assert(std::atomic_ref<void*>::required_alignment <= alignof(MutexHandle));
            static_assert(std::atomic_ref<void*>::required_alignment <= alignof(ConditionHandle));
            static_assert(std::atomic_ref<void*>::required_alignment <= alignof(SemaphoreHandle));

            TX_MUTEX initialization_mutex;
            TX_QUEUE cleanup_queue;
            TX_THREAD reaper_thread;
            TX_TIMER rollover_timer;
            alignas(STACK_ALIGNMENT) UCHAR reaper_stack[REAPER_STACK_SIZE];
            ULONG cleanup_queue_storage[CLEANUP_QUEUE_DEPTH];
            CHAR initialization_mutex_name[] = "std init";
            CHAR cleanup_queue_name[] = "std cleanup";
            CHAR reaper_thread_name[] = "std reaper";
            CHAR rollover_timer_name[] = "std clock wrap";
            CHAR mutex_name[] = "std mutex";
            CHAR condition_mutex_name[] = "std condition";
            CHAR waiter_semaphore_name[] = "std cv waiter";
            CHAR semaphore_name[] = "std semaphore";
            CHAR completion_name[] = "std completion";
            CHAR lifecycle_mutex_name[] = "std lifecycle";
            bool initialized{};
            std::uint32_t next_thread_number{};
            ULONG next_registry_id{ 1U };
            ThreadControl* registry_head{};
            KeyDefinition keys[RUNTIME_THREAD_KEY_COUNT];
            std::uint32_t last_tick{};
            std::uint64_t tick_epoch{};
            std::int64_t system_clock_epoch_nanoseconds{};
            std::uint64_t system_clock_epoch_ticks{};
            std::atomic<std::int64_t> last_system_time_nanoseconds{};
            std::shared_ptr<hal::ITimer> high_resolution_timer{};
            HighResolutionCounter high_resolution_counter_epoch{};
            bool system_clock_epoch_available{};
            std::atomic_bool high_resolution_counter_available{};

            constexpr unsigned int KEY_INDEX_BITS{ 8U };
            constexpr unsigned int KEY_INDEX_MASK{ (1U << KEY_INDEX_BITS) - 1U };
            constexpr unsigned int KEY_GENERATION_MASK{ ~KEY_INDEX_MASK };
            static_assert(RUNTIME_THREAD_KEY_COUNT > 0U);
            static_assert(RUNTIME_THREAD_KEY_COUNT <= (1U << KEY_INDEX_BITS));

            [[nodiscard]] KeyHandle make_key(std::size_t index, unsigned int generation) noexcept
            {
                return (generation << KEY_INDEX_BITS) | static_cast<unsigned int>(index);
            }

            [[nodiscard]] std::size_t key_index(KeyHandle key) noexcept { return key & KEY_INDEX_MASK; }

            [[nodiscard]] unsigned int key_generation(KeyHandle key) noexcept
            {
                return key >> KEY_INDEX_BITS;
            }

            [[nodiscard]] bool validate_thread_priority(int32_t requested_prio, UINT& validated_prio) noexcept
            {
                if (requested_prio == -1) {
                    requested_prio = RUNTIME_STD_THREAD_PRIORITY;
                }
                if (requested_prio < 0 || requested_prio >= static_cast<int32_t>(TX_MAX_PRIORITIES)) {
                    return false;
                }
                validated_prio = static_cast<UINT>(requested_prio);
                return true;
            }

            [[nodiscard]] bool normalize_thread_stack_size(std::size_t requested_size,
                                                           ULONG& normalized_size) noexcept
            {
                if (requested_size == 0U) {
                    requested_size = RUNTIME_STD_THREAD_STACK_SIZE;
                }
                constexpr std::size_t alignment_mask{ STACK_ALIGNMENT - 1U };
                if (requested_size < TX_MINIMUM_STACK ||
                    requested_size > std::numeric_limits<std::size_t>::max() - alignment_mask) {
                    return false;
                }
                requested_size = (requested_size + alignment_mask) & ~alignment_mask;
                if (requested_size > std::numeric_limits<ULONG>::max()) {
                    return false;
                }
                normalized_size = static_cast<ULONG>(requested_size);
                return true;
            }

            [[nodiscard]]
            std::optional<std::string_view> snake_to_camel(std::string_view input,
                                                           std::span<char> output) noexcept
            {
                auto written{ 0UZ };
                auto capitalize_next{ true };

                for (auto c : input) {
                    if (c == '_') {
                        capitalize_next = written != 0;
                        continue;
                    }
                    if (written == output.size()) {
                        return std::nullopt;
                    }
                    output[written++] = capitalize_next
                                          ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                                          : c;
                    capitalize_next = false;
                }

                return std::string_view{ output.data(), written };
            }

            [[nodiscard]] bool valid_high_resolution_counter(const HighResolutionCounter& counter) noexcept
            {
                return counter.ticks_per_second != 0U && counter.ticks_per_second <= NANOSECONDS_PER_SECOND &&
                       (counter.modulus == 0U || counter.ticks < counter.modulus);
            }

            [[nodiscard]] bool read_high_resolution_counter(HighResolutionCounter& counter) noexcept
            {
                if (!high_resolution_timer) {
                    return false;
                }

                const std::uint32_t state_version{ high_resolution_timer->getStateVersion() };
                if (!high_resolution_timer->isRunning()) {
                    return false;
                }
                counter.ticks = high_resolution_timer->getCounter();
                counter.ticks_per_second = high_resolution_timer->getTickFrequencyHz();
                counter.modulus = static_cast<std::uint64_t>(high_resolution_timer->getAutoReload()) + 1U;
                counter.state_version = state_version;
                return high_resolution_timer->isRunning() &&
                       high_resolution_timer->getStateVersion() == state_version &&
                       valid_high_resolution_counter(counter);
            }

            [[nodiscard]] bool thread_ticks_to_counter_ticks(std::uint64_t ticks,
                                                             std::uint64_t frequency,
                                                             std::uint64_t& result) noexcept
            {
                const std::uint64_t seconds{ ticks / TX_TIMER_TICKS_PER_SECOND };
                const std::uint64_t remainder{ ticks % TX_TIMER_TICKS_PER_SECOND };
                const std::uint64_t fractional_ticks{ remainder * frequency / TX_TIMER_TICKS_PER_SECOND };
                if (seconds > (std::numeric_limits<std::uint64_t>::max() - fractional_ticks) / frequency) {
                    return false;
                }
                result = seconds * frequency + fractional_ticks;
                return true;
            }

            [[nodiscard]] bool counter_ticks_to_nanoseconds(std::uint64_t ticks,
                                                            std::uint64_t frequency,
                                                            std::int64_t& result) noexcept
            {
                const std::uint64_t seconds{ ticks / frequency };
                const std::uint64_t remainder{ ticks % frequency };
                constexpr auto maximum{ static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max()) };
                const std::uint64_t fractional_nanoseconds{ remainder * NANOSECONDS_PER_SECOND / frequency };
                if (seconds > (maximum - fractional_nanoseconds) / NANOSECONDS_PER_SECOND) {
                    return false;
                }
                result = static_cast<std::int64_t>(seconds * NANOSECONDS_PER_SECOND + fractional_nanoseconds);
                return true;
            }

            [[nodiscard]] bool high_resolution_elapsed_nanoseconds(std::uint64_t current_thread_ticks,
                                                                   std::int64_t& result) noexcept
            {
                if (!high_resolution_counter_available.load(std::memory_order_relaxed)) {
                    return false;
                }

                HighResolutionCounter current{};
                if (!read_high_resolution_counter(current) ||
                    current.state_version != high_resolution_counter_epoch.state_version ||
                    current.ticks_per_second != high_resolution_counter_epoch.ticks_per_second ||
                    current.modulus != high_resolution_counter_epoch.modulus) {
                    high_resolution_counter_available.store(false, std::memory_order_relaxed);
                    return false;
                }

                std::uint64_t elapsed_counter_ticks{};
                if (current.modulus == 0U) {
                    if (current.ticks < high_resolution_counter_epoch.ticks) {
                        return false;
                    }
                    elapsed_counter_ticks = current.ticks - high_resolution_counter_epoch.ticks;
                }
                else {
                    const std::uint64_t modular_elapsed{
                        current.ticks >= high_resolution_counter_epoch.ticks
                          ? current.ticks - high_resolution_counter_epoch.ticks
                          : current.modulus - high_resolution_counter_epoch.ticks + current.ticks
                    };
                    std::uint64_t coarse_elapsed{};
                    if (!thread_ticks_to_counter_ticks(current_thread_ticks - system_clock_epoch_ticks,
                                                       current.ticks_per_second,
                                                       coarse_elapsed)) {
                        return false;
                    }

                    std::uint64_t wraps{};
                    if (coarse_elapsed >= modular_elapsed) {
                        const std::uint64_t difference{ coarse_elapsed - modular_elapsed };
                        wraps = difference / current.modulus;
                        const std::uint64_t remainder{ difference % current.modulus };
                        if (remainder >= current.modulus - remainder) {
                            ++wraps;
                        }
                    }
                    else if (modular_elapsed - coarse_elapsed > current.modulus / 2U) {
                        // A reset or stopped timer can look like a wrap. Reject
                        // it when ThreadX's coarse clock cannot corroborate it.
                        return false;
                    }

                    if (wraps >
                        (std::numeric_limits<std::uint64_t>::max() - modular_elapsed) / current.modulus) {
                        return false;
                    }
                    elapsed_counter_ticks = modular_elapsed + wraps * current.modulus;
                }

                return counter_ticks_to_nanoseconds(elapsed_counter_ticks, current.ticks_per_second, result);
            }

            [[nodiscard]] std::int64_t publish_system_time(std::int64_t candidate) noexcept
            {
                std::int64_t previous{ last_system_time_nanoseconds.load(std::memory_order_relaxed) };
                while (candidate > previous &&
                       !last_system_time_nanoseconds.compare_exchange_weak(
                         previous, candidate, std::memory_order_relaxed, std::memory_order_relaxed)) {
                }
                return candidate > previous ? candidate : previous;
            }

            [[nodiscard]] bool interrupt_context() noexcept
            {
#if defined(__arm__) || defined(__thumb__)
                std::uint32_t ipsr{};
                asm volatile("mrs %0, ipsr" : "=r"(ipsr));
                return ipsr != 0U;
#else
                return false;
#endif
            }

            [[nodiscard]] int require_thread_context() noexcept
            {
                if (!initialized) {
                    return EAGAIN;
                }
                if (interrupt_context() || tx_thread_identify() == TX_NULL) {
                    return EPERM;
                }
                return 0;
            }

            void raw_mutex_get(TX_MUTEX* mutex)
            {
                const UINT status{ tx_mutex_get(mutex, TX_WAIT_FOREVER) };
                if (status != TX_SUCCESS) {
                    fatal_error("tx_mutex_get", status);
                }
            }

            void raw_mutex_put(TX_MUTEX* mutex)
            {
                const UINT status{ tx_mutex_put(mutex) };
                if (status != TX_SUCCESS) {
                    fatal_error("tx_mutex_put", status);
                }
            }

            template<typename Implementation, typename Handle>
            [[nodiscard]] Implementation* load_implementation(Handle* handle) noexcept
            {
                return static_cast<Implementation*>(
                  std::atomic_ref<void*>{ handle->implementation }.load(std::memory_order_acquire));
            }

            template<typename Handle>
            void store_implementation(Handle* handle, void* implementation) noexcept
            {
                std::atomic_ref<void*>{ handle->implementation }.store(implementation,
                                                                       std::memory_order_release);
            }

            template<typename Handle>
            [[nodiscard]] MutexImplementation* ensure_mutex(Handle* handle) noexcept
            {
                if (handle == nullptr || require_thread_context() != 0) {
                    return nullptr;
                }
                if (auto* implementation{ load_implementation<MutexImplementation>(handle) };
                    implementation != nullptr) {
                    return implementation;
                }

                raw_mutex_get(&initialization_mutex);
                auto* result{ load_implementation<MutexImplementation>(handle) };
                if (result == nullptr) {
                    auto* candidate{ new (std::nothrow) MutexImplementation{} };
                    if (candidate != nullptr) {
                        if (tx_mutex_create(&candidate->mutex, mutex_name, TX_INHERIT) == TX_SUCCESS) {
                            store_implementation(handle, candidate);
                            result = candidate;
                        }
                        else {
                            delete candidate;
                        }
                    }
                }
                raw_mutex_put(&initialization_mutex);
                return result;
            }

            template<typename Handle>
            int destroy_mutex(Handle* handle) noexcept
            {
                if (handle == nullptr) {
                    return EINVAL;
                }
                auto* implementation{ load_implementation<MutexImplementation>(handle) };
                if (implementation == nullptr) {
                    return 0;
                }
                const UINT status{ tx_mutex_delete(&implementation->mutex) };
                if (status != TX_SUCCESS) {
                    return status == TX_DELETE_ERROR ? EBUSY : EINVAL;
                }
                store_implementation(handle, nullptr);
                delete implementation;
                return 0;
            }

            template<typename Handle>
            int lock_mutex(Handle* handle, bool recursive, ULONG wait_option) noexcept
            {
                const int context_status{ require_thread_context() };
                if (context_status != 0) {
                    return context_status;
                }
                auto* implementation{ ensure_mutex(handle) };
                if (implementation == nullptr) {
                    return ENOMEM;
                }

                if (!recursive && implementation->mutex.tx_mutex_owner == tx_thread_identify()) {
                    return wait_option == TX_NO_WAIT ? EBUSY : EDEADLK;
                }

                const UINT status{ tx_mutex_get(&implementation->mutex, wait_option) };
                if (status == TX_SUCCESS) {
                    return 0;
                }
                if (status == TX_NOT_AVAILABLE) {
                    return EBUSY;
                }
                if (status == TX_NO_INSTANCE) {
                    return ETIMEDOUT;
                }
                return EINVAL;
            }

            template<typename Handle>
            int unlock_mutex(Handle* handle) noexcept
            {
                const int context_status{ require_thread_context() };
                if (context_status != 0) {
                    return context_status;
                }
                if (handle == nullptr) {
                    return EPERM;
                }
                auto* implementation{ load_implementation<MutexImplementation>(handle) };
                if (implementation == nullptr) {
                    return EPERM;
                }
                return tx_mutex_put(&implementation->mutex) == TX_SUCCESS ? 0 : EPERM;
            }

            [[nodiscard]] std::int64_t timespec_to_nanoseconds(const TimePoint& time) noexcept
            {
                constexpr auto maximum_seconds{ std::numeric_limits<std::int64_t>::max() /
                                                static_cast<std::int64_t>(NANOSECONDS_PER_SECOND) };
                if (time.tv_sec >= maximum_seconds) {
                    return std::numeric_limits<std::int64_t>::max();
                }
                if (time.tv_sec < 0) {
                    return -1;
                }
                return static_cast<std::int64_t>(time.tv_sec) *
                         static_cast<std::int64_t>(NANOSECONDS_PER_SECOND) +
                       std::clamp<std::int64_t>(time.tv_nsec, 0, NANOSECONDS_PER_SECOND - 1U);
            }

            [[nodiscard]] ULONG deadline_wait_ticks(const TimePoint* deadline) noexcept
            {
                if (deadline == nullptr) {
                    return TX_NO_WAIT;
                }
                const std::int64_t deadline_ns{ timespec_to_nanoseconds(*deadline) };
                const std::int64_t now_ns{ system_time_nanoseconds() };
                if (deadline_ns <= now_ns) {
                    return TX_NO_WAIT;
                }
                const auto remaining{ static_cast<std::uint64_t>(deadline_ns - now_ns) };
                return std::min(duration_to_ticks(remaining), MAX_FINITE_WAIT);
            }

            template<typename WaitFunction>
            int wait_until(const TimePoint* deadline, WaitFunction&& wait_function) noexcept
            {
                while (true) {
                    const ULONG wait_ticks{ deadline_wait_ticks(deadline) };
                    if (wait_ticks == TX_NO_WAIT) {
                        return wait_function(TX_NO_WAIT) == TX_SUCCESS ? 0 : ETIMEDOUT;
                    }
                    const UINT status{ wait_function(wait_ticks) };
                    if (status == TX_SUCCESS) {
                        return 0;
                    }
                    if (status != TX_NO_INSTANCE && status != TX_NOT_AVAILABLE) {
                        return EINVAL;
                    }

                    // A ThreadX tick timeout may occur just before the absolute
                    // C++ deadline because the wait begins part-way through a
                    // tick. Recompute the remaining time instead of reporting
                    // an early timeout. The TX_NO_WAIT path above performs one
                    // final acquisition attempt at or after the deadline.
                }
            }

            [[nodiscard]] ConditionImplementation* ensure_condition(ConditionHandle* handle) noexcept
            {
                if (handle == nullptr || require_thread_context() != 0) {
                    return nullptr;
                }
                if (auto* implementation{ load_implementation<ConditionImplementation>(handle) };
                    implementation != nullptr) {
                    return implementation;
                }

                raw_mutex_get(&initialization_mutex);
                auto* result{ load_implementation<ConditionImplementation>(handle) };
                if (result == nullptr) {
                    auto* candidate{ new (std::nothrow) ConditionImplementation{} };
                    if (candidate != nullptr) {
                        if (tx_mutex_create(&candidate->mutex, condition_mutex_name, TX_INHERIT) ==
                            TX_SUCCESS) {
                            store_implementation(handle, candidate);
                            result = candidate;
                        }
                        else {
                            delete candidate;
                        }
                    }
                }
                raw_mutex_put(&initialization_mutex);
                return result;
            }

            void append_waiter(ConditionImplementation* condition, Waiter* waiter) noexcept
            {
                waiter->previous = condition->last;
                waiter->next = nullptr;
                waiter->linked = true;
                if (condition->last != nullptr) {
                    condition->last->next = waiter;
                }
                else {
                    condition->first = waiter;
                }
                condition->last = waiter;
            }

            void remove_waiter(ConditionImplementation* condition, Waiter* waiter) noexcept
            {
                if (!waiter->linked) {
                    return;
                }
                if (waiter->previous != nullptr) {
                    waiter->previous->next = waiter->next;
                }
                else {
                    condition->first = waiter->next;
                }
                if (waiter->next != nullptr) {
                    waiter->next->previous = waiter->previous;
                }
                else {
                    condition->last = waiter->previous;
                }
                waiter->linked = false;
            }

            [[nodiscard]] SemaphoreImplementation* ensure_semaphore(SemaphoreHandle* handle) noexcept
            {
                if (handle == nullptr || require_thread_context() != 0) {
                    return nullptr;
                }
                if (auto* implementation{ load_implementation<SemaphoreImplementation>(handle) };
                    implementation != nullptr) {
                    return implementation;
                }

                raw_mutex_get(&initialization_mutex);
                auto* result{ load_implementation<SemaphoreImplementation>(handle) };
                if (result == nullptr) {
                    auto* candidate{ new (std::nothrow) SemaphoreImplementation{} };
                    if (candidate != nullptr) {
                        if (tx_semaphore_create(&candidate->semaphore,
                                                semaphore_name,
                                                static_cast<ULONG>(handle->initial_count)) == TX_SUCCESS) {
                            store_implementation(handle, candidate);
                            result = candidate;
                        }
                        else {
                            delete candidate;
                        }
                    }
                }
                raw_mutex_put(&initialization_mutex);
                return result;
            }

            void register_thread(ThreadControl* control)
            {
                raw_mutex_get(&initialization_mutex);
                control->registry_id = next_registry_id++;
                if (next_registry_id == 0U) {
                    next_registry_id = 1U;
                }
                control->registry_next = registry_head;
                registry_head = control;
                raw_mutex_put(&initialization_mutex);
            }

            void unregister_thread(ThreadControl* control)
            {
                raw_mutex_get(&initialization_mutex);
                ThreadControl** current{ &registry_head };
                while (*current != nullptr && *current != control) {
                    current = &(*current)->registry_next;
                }
                if (*current == control) {
                    *current = control->registry_next;
                }
                raw_mutex_put(&initialization_mutex);
            }

            [[nodiscard]] ThreadControl* find_thread(ULONG registry_id)
            {
                raw_mutex_get(&initialization_mutex);
                ThreadControl* current{ registry_head };
                while (current != nullptr && current->registry_id != registry_id) {
                    current = current->registry_next;
                }
                raw_mutex_put(&initialization_mutex);
                return current;
            }

            void enqueue_cleanup(ThreadControl* control)
            {
                const ULONG message{ control->registry_id };
                const UINT status{ tx_queue_send(
                  &cleanup_queue, const_cast<ULONG*>(&message), TX_WAIT_FOREVER) };
                if (status != TX_SUCCESS) {
                    fatal_error("tx_queue_send", status);
                }
            }

            void thread_entry(ULONG parameter)
            {
                auto* control{ find_thread(parameter) };
                if (control == nullptr) {
                    fatal_error("thread registry", TX_PTR_ERROR);
                }
                static_cast<void>(control->entry(control->argument));

                // Exit callbacks may themselves join other threads. Do not
                // enqueue this thread until they finish: the single reaper
                // must remain available to service those nested joins.
                cleanup_thread_runtime(&control->thread);

                raw_mutex_get(&control->lifecycle_mutex);
                control->finished = true;
                const bool enqueue{ control->detached && !control->cleanup_queued };
                if (enqueue) {
                    control->cleanup_queued = true;
                }
                raw_mutex_put(&control->lifecycle_mutex);

                if (tx_semaphore_put(&control->completion) != TX_SUCCESS) {
                    fatal_error("tx_semaphore_put", TX_SEMAPHORE_ERROR);
                }
                if (enqueue) {
                    enqueue_cleanup(control);
                }
            }

            void reaper_entry(ULONG)
            {
                while (true) {
                    ULONG message{};
                    const UINT receive_status{ tx_queue_receive(&cleanup_queue, &message, TX_WAIT_FOREVER) };
                    if (receive_status != TX_SUCCESS) {
                        fatal_error("tx_queue_receive", receive_status);
                    }
                    auto* control{ find_thread(message) };
                    if (control == nullptr) {
                        fatal_error("cleanup registry", TX_PTR_ERROR);
                    }

                    UINT state{};
                    while (true) {
                        const UINT info_status{ tx_thread_info_get(&control->thread,
                                                                   nullptr,
                                                                   &state,
                                                                   nullptr,
                                                                   nullptr,
                                                                   nullptr,
                                                                   nullptr,
                                                                   nullptr,
                                                                   nullptr) };
                        if (info_status != TX_SUCCESS) {
                            fatal_error("tx_thread_info_get", info_status);
                        }
                        if (state == TX_COMPLETED || state == TX_TERMINATED) {
                            break;
                        }
                        tx_thread_sleep(1U);
                    }

                    TX_SEMAPHORE* const cleanup_ack{ control->cleanup_ack };
                    void* const stack{ control->stack };
                    if (tx_thread_delete(&control->thread) != TX_SUCCESS ||
                        tx_mutex_delete(&control->lifecycle_mutex) != TX_SUCCESS ||
                        tx_semaphore_delete(&control->completion) != TX_SUCCESS) {
                        fatal_error("thread cleanup", TX_DELETE_ERROR);
                    }
                    unregister_thread(control);
                    delete control;
                    ::operator delete[](stack);
                    if (cleanup_ack != nullptr && tx_semaphore_put(cleanup_ack) != TX_SUCCESS) {
                        fatal_error("cleanup acknowledgement", TX_SEMAPHORE_ERROR);
                    }
                }
            }

            void rollover_timer_entry(ULONG) { static_cast<void>(steady_ticks()); }
        }

        bool active() noexcept { return initialized; }

        bool in_thread_context() noexcept
        {
            return initialized && !interrupt_context() && tx_thread_identify() != TX_NULL;
        }

        int thread_create(ThreadHandle* thread,
                          void* (*entry)(void*),
                          void* argument,
                          const thread::Attributes& attributes) noexcept
        {
            const int context_status{ require_thread_context() };
            if (context_status != 0) {
                return context_status;
            }
            if (thread == nullptr || entry == nullptr) {
                return EINVAL;
            }
            ULONG normalized_stack_size{};
            if (!normalize_thread_stack_size(attributes.stack_size, normalized_stack_size)) {
                return EINVAL;
            }
            UINT validated_priority{};
            if (!validate_thread_priority(attributes.priority, validated_priority)) {
                return EINVAL;
            }

            auto* control{ new (std::nothrow) ThreadControl{} };
            if (control == nullptr) {
                return EAGAIN;
            }
            control->stack = ::operator new[](static_cast<std::size_t>(normalized_stack_size), std::nothrow);
            if (control->stack == nullptr) {
                delete control;
                return EAGAIN;
            }
            control->entry = entry;
            control->argument = argument;

            if (attributes.name.empty()) {
                const ULONG old_posture{ tx_interrupt_control(TX_INT_DISABLE) };
                const std::uint32_t number{ next_thread_number++ };
                static_cast<void>(tx_interrupt_control(old_posture));
                static_cast<void>(std::snprintf(control->name,
                                                sizeof(control->name),
                                                "std::thread %lu",
                                                static_cast<unsigned long>(number)));
            }
            else {
                std::array<char, sizeof(control->name)> convert_buff;
                auto converted_name{ snake_to_camel(attributes.name, convert_buff).value_or("Unknown") };
                static_cast<void>(
                  std::snprintf(control->name,
                                sizeof(control->name),
                                "%.*s Thread",
                                static_cast<int>(converted_name.size()),
                                converted_name.data()));
            }

            if (tx_mutex_create(&control->lifecycle_mutex, lifecycle_mutex_name, TX_INHERIT) != TX_SUCCESS) {
                ::operator delete[](control->stack);
                delete control;
                return EAGAIN;
            }
            if (tx_semaphore_create(&control->completion, completion_name, 0U) != TX_SUCCESS) {
                static_cast<void>(tx_mutex_delete(&control->lifecycle_mutex));
                ::operator delete[](control->stack);
                delete control;
                return EAGAIN;
            }

            register_thread(control);

            const UINT status{ tx_thread_create(&control->thread,
                                                control->name,
                                                thread_entry,
                                                control->registry_id,
                                                control->stack,
                                                normalized_stack_size,
                                                validated_priority,
                                                validated_priority,
                                                TX_NO_TIME_SLICE,
                                                TX_AUTO_START) };
            if (status != TX_SUCCESS) {
                unregister_thread(control);
                static_cast<void>(tx_semaphore_delete(&control->completion));
                static_cast<void>(tx_mutex_delete(&control->lifecycle_mutex));
                ::operator delete[](control->stack);
                delete control;
                return EAGAIN;
            }
            *thread = &control->thread;
            return 0;
        }

        int thread_join(ThreadHandle thread, void** result) noexcept
        {
            const int context_status{ require_thread_context() };
            if (context_status != 0) {
                return context_status;
            }
            if (thread == nullptr) {
                return EINVAL;
            }
            if (thread == tx_thread_identify()) {
                return EDEADLK;
            }
            auto* control{ reinterpret_cast<ThreadControl*>(thread) };
            if (tx_semaphore_get(&control->completion, TX_WAIT_FOREVER) != TX_SUCCESS) {
                return EINVAL;
            }

            TX_SEMAPHORE acknowledgement{};
            CHAR acknowledgement_name[] = "std join ack";
            if (tx_semaphore_create(&acknowledgement, acknowledgement_name, 0U) != TX_SUCCESS) {
                return EAGAIN;
            }

            raw_mutex_get(&control->lifecycle_mutex);
            if (control->detached || control->cleanup_queued) {
                raw_mutex_put(&control->lifecycle_mutex);
                static_cast<void>(tx_semaphore_delete(&acknowledgement));
                return EINVAL;
            }
            control->cleanup_ack = &acknowledgement;
            control->cleanup_queued = true;
            raw_mutex_put(&control->lifecycle_mutex);
            enqueue_cleanup(control);

            if (tx_semaphore_get(&acknowledgement, TX_WAIT_FOREVER) != TX_SUCCESS) {
                fatal_error("join acknowledgement", TX_SEMAPHORE_ERROR);
            }
            if (tx_semaphore_delete(&acknowledgement) != TX_SUCCESS) {
                fatal_error("join acknowledgement delete", TX_DELETE_ERROR);
            }
            if (result != nullptr) {
                *result = nullptr;
            }
            return 0;
        }

        int thread_detach(ThreadHandle thread) noexcept
        {
            const int context_status{ require_thread_context() };
            if (context_status != 0) {
                return context_status;
            }
            if (thread == nullptr) {
                return EINVAL;
            }
            auto* control{ reinterpret_cast<ThreadControl*>(thread) };
            raw_mutex_get(&control->lifecycle_mutex);
            if (control->detached || control->cleanup_queued) {
                raw_mutex_put(&control->lifecycle_mutex);
                return EINVAL;
            }
            control->detached = true;
            const bool enqueue{ control->finished };
            if (enqueue) {
                control->cleanup_queued = true;
            }
            raw_mutex_put(&control->lifecycle_mutex);
            if (enqueue) {
                enqueue_cleanup(control);
            }
            return 0;
        }

        ThreadHandle thread_self() noexcept { return tx_thread_identify(); }

        int thread_yield() noexcept
        {
            if (require_thread_context() != 0) {
                return EPERM;
            }
            tx_thread_relinquish();
            return 0;
        }

        int key_create(KeyHandle* key, void (*destructor)(void*)) noexcept
        {
            const int context_status{ require_thread_context() };
            if (context_status != 0 || key == nullptr) {
                return context_status != 0 ? context_status : EINVAL;
            }

            raw_mutex_get(&initialization_mutex);
            for (std::size_t index{}; index < RUNTIME_THREAD_KEY_COUNT; ++index) {
                if (!keys[index].allocated) {
                    keys[index].allocated = true;
                    keys[index].destructor = destructor;
                    keys[index].generation =
                      (keys[index].generation + 1U) & (KEY_GENERATION_MASK >> KEY_INDEX_BITS);
                    if (keys[index].generation == 0U) {
                        ++keys[index].generation;
                    }
                    *key = make_key(index, keys[index].generation);
                    raw_mutex_put(&initialization_mutex);
                    return 0;
                }
            }
            raw_mutex_put(&initialization_mutex);
            return EAGAIN;
        }

        int key_delete(KeyHandle key) noexcept
        {
            const int context_status{ require_thread_context() };
            const std::size_t index{ key_index(key) };
            const unsigned int generation{ key_generation(key) };
            if (context_status != 0 || index >= RUNTIME_THREAD_KEY_COUNT || generation == 0U) {
                return context_status != 0 ? context_status : EINVAL;
            }

            raw_mutex_get(&initialization_mutex);
            if (!keys[index].allocated || keys[index].generation != generation) {
                raw_mutex_put(&initialization_mutex);
                return EINVAL;
            }
            keys[index].allocated = false;
            keys[index].destructor = nullptr;
            raw_mutex_put(&initialization_mutex);
            return 0;
        }

        void* key_get(KeyHandle key) noexcept
        {
            const std::size_t index{ key_index(key) };
            const unsigned int generation{ key_generation(key) };
            if (require_thread_context() != 0 || index >= RUNTIME_THREAD_KEY_COUNT || generation == 0U) {
                return nullptr;
            }

            raw_mutex_get(&initialization_mutex);
            const bool allocated{ keys[index].allocated && keys[index].generation == generation };
            raw_mutex_put(&initialization_mutex);
            TX_THREAD* const current{ tx_thread_identify() };
            if (!allocated || current->tx_thread_runtime_tls_generations[index] != generation) {
                return nullptr;
            }
            return current->tx_thread_runtime_tls_values[index];
        }

        int key_set(KeyHandle key, const void* value) noexcept
        {
            const int context_status{ require_thread_context() };
            const std::size_t index{ key_index(key) };
            const unsigned int generation{ key_generation(key) };
            if (context_status != 0 || index >= RUNTIME_THREAD_KEY_COUNT || generation == 0U) {
                return context_status != 0 ? context_status : EINVAL;
            }

            raw_mutex_get(&initialization_mutex);
            const bool allocated{ keys[index].allocated && keys[index].generation == generation };
            raw_mutex_put(&initialization_mutex);
            if (!allocated) {
                return EINVAL;
            }
            TX_THREAD* const current{ tx_thread_identify() };
            current->tx_thread_runtime_tls_generations[index] = generation;
            current->tx_thread_runtime_tls_values[index] = const_cast<void*>(value);
            return 0;
        }

        void run_thread_specific_destructors(TX_THREAD* thread) noexcept
        {
            if (!initialized || thread == nullptr) {
                return;
            }

            constexpr unsigned int destructor_iterations{ 4U };
            for (unsigned int pass{}; pass < destructor_iterations; ++pass) {
                bool invoked{};
                for (std::size_t index{}; index < RUNTIME_THREAD_KEY_COUNT; ++index) {
                    raw_mutex_get(&initialization_mutex);
                    auto* const destructor{ keys[index].allocated ? keys[index].destructor : nullptr };
                    const unsigned int generation{ keys[index].generation };
                    raw_mutex_put(&initialization_mutex);

                    void* const value{ thread->tx_thread_runtime_tls_values[index] };
                    if (destructor != nullptr && value != nullptr &&
                        thread->tx_thread_runtime_tls_generations[index] == generation) {
                        thread->tx_thread_runtime_tls_values[index] = nullptr;
                        invoked = true;
                        destructor(value);
                    }
                }
                if (!invoked) {
                    return;
                }
            }
        }

        void mutex_init(MutexHandle* mutex) noexcept
        {
            if (mutex != nullptr) {
                store_implementation(mutex, nullptr);
            }
        }

        void recursive_mutex_init(RecursiveMutexHandle* mutex) noexcept
        {
            if (mutex != nullptr) {
                store_implementation(mutex, nullptr);
            }
        }

        int mutex_destroy(MutexHandle* mutex) noexcept { return destroy_mutex(mutex); }
        int recursive_mutex_destroy(RecursiveMutexHandle* mutex) noexcept { return destroy_mutex(mutex); }
        int mutex_lock(MutexHandle* mutex) noexcept { return lock_mutex(mutex, false, TX_WAIT_FOREVER); }
        int recursive_mutex_lock(RecursiveMutexHandle* mutex) noexcept
        {
            return lock_mutex(mutex, true, TX_WAIT_FOREVER);
        }
        int mutex_try_lock(MutexHandle* mutex) noexcept { return lock_mutex(mutex, false, TX_NO_WAIT); }
        int recursive_mutex_try_lock(RecursiveMutexHandle* mutex) noexcept
        {
            return lock_mutex(mutex, true, TX_NO_WAIT);
        }

        int mutex_timed_lock(MutexHandle* mutex, const TimePoint* deadline) noexcept
        {
            const int context_status{ require_thread_context() };
            if (context_status != 0) {
                return context_status;
            }
            auto* implementation{ ensure_mutex(mutex) };
            if (implementation == nullptr) {
                return ENOMEM;
            }
            if (implementation->mutex.tx_mutex_owner == tx_thread_identify()) {
                return EDEADLK;
            }
            return wait_until(deadline,
                              [&](ULONG ticks) { return tx_mutex_get(&implementation->mutex, ticks); });
        }

        int recursive_mutex_timed_lock(RecursiveMutexHandle* mutex, const TimePoint* deadline) noexcept
        {
            const int context_status{ require_thread_context() };
            if (context_status != 0) {
                return context_status;
            }
            auto* implementation{ ensure_mutex(mutex) };
            if (implementation == nullptr) {
                return ENOMEM;
            }
            return wait_until(deadline,
                              [&](ULONG ticks) { return tx_mutex_get(&implementation->mutex, ticks); });
        }

        int mutex_unlock(MutexHandle* mutex) noexcept { return unlock_mutex(mutex); }
        int recursive_mutex_unlock(RecursiveMutexHandle* mutex) noexcept { return unlock_mutex(mutex); }

        void condition_init(ConditionHandle* condition) noexcept
        {
            if (condition != nullptr) {
                store_implementation(condition, nullptr);
            }
        }

        int condition_destroy(ConditionHandle* condition) noexcept
        {
            if (condition == nullptr) {
                return EINVAL;
            }
            auto* implementation{ load_implementation<ConditionImplementation>(condition) };
            if (implementation == nullptr) {
                return 0;
            }
            raw_mutex_get(&implementation->mutex);
            const bool busy{ implementation->first != nullptr };
            raw_mutex_put(&implementation->mutex);
            if (busy) {
                return EBUSY;
            }
            if (tx_mutex_delete(&implementation->mutex) != TX_SUCCESS) {
                return EBUSY;
            }
            store_implementation(condition, nullptr);
            delete implementation;
            return 0;
        }

        namespace
        {
            int condition_wait_common(ConditionHandle* condition,
                                      MutexHandle* mutex,
                                      const TimePoint* deadline) noexcept
            {
                const int context_status{ require_thread_context() };
                if (context_status != 0) {
                    return context_status;
                }
                auto* condition_implementation{ ensure_condition(condition) };
                if (condition_implementation == nullptr || mutex == nullptr ||
                    load_implementation<MutexImplementation>(mutex) == nullptr) {
                    return EINVAL;
                }

                Waiter waiter{};
                if (tx_semaphore_create(&waiter.semaphore, waiter_semaphore_name, 0U) != TX_SUCCESS) {
                    return EAGAIN;
                }

                raw_mutex_get(&condition_implementation->mutex);
                append_waiter(condition_implementation, &waiter);
                const int unlock_status{ mutex_unlock(mutex) };
                raw_mutex_put(&condition_implementation->mutex);
                if (unlock_status != 0) {
                    raw_mutex_get(&condition_implementation->mutex);
                    remove_waiter(condition_implementation, &waiter);
                    raw_mutex_put(&condition_implementation->mutex);
                    static_cast<void>(tx_semaphore_delete(&waiter.semaphore));
                    return unlock_status;
                }

                int wait_status{};
                if (deadline == nullptr) {
                    wait_status =
                      tx_semaphore_get(&waiter.semaphore, TX_WAIT_FOREVER) == TX_SUCCESS ? 0 : EINVAL;
                }
                else {
                    wait_status = wait_until(
                      deadline, [&](ULONG ticks) { return tx_semaphore_get(&waiter.semaphore, ticks); });
                }

                if (wait_status != 0) {
                    raw_mutex_get(&condition_implementation->mutex);
                    if (waiter.linked) {
                        remove_waiter(condition_implementation, &waiter);
                    }
                    else {
                        const UINT notification_status{ tx_semaphore_get(&waiter.semaphore, TX_NO_WAIT) };
                        if (notification_status == TX_SUCCESS) {
                            wait_status = 0;
                        }
                    }
                    raw_mutex_put(&condition_implementation->mutex);
                }

                if (tx_semaphore_delete(&waiter.semaphore) != TX_SUCCESS) {
                    fatal_error("condition waiter delete", TX_DELETE_ERROR);
                }
                const int lock_status{ mutex_lock(mutex) };
                return lock_status == 0 ? wait_status : lock_status;
            }
        }

        int condition_wait(ConditionHandle* condition, MutexHandle* mutex) noexcept
        {
            return condition_wait_common(condition, mutex, nullptr);
        }

        int condition_wait_recursive(ConditionHandle* condition, RecursiveMutexHandle* mutex) noexcept
        {
            if (mutex == nullptr) {
                return EINVAL;
            }
            const auto* implementation{ load_implementation<MutexImplementation>(mutex) };
            if (implementation == nullptr) {
                return EINVAL;
            }
            if (implementation->mutex.tx_mutex_owner != tx_thread_identify() ||
                implementation->mutex.tx_mutex_ownership_count != 1U) {
                return EINVAL;
            }
            return condition_wait_common(condition, mutex, nullptr);
        }

        int condition_timed_wait(ConditionHandle* condition,
                                 MutexHandle* mutex,
                                 const TimePoint* deadline) noexcept
        {
            return condition_wait_common(condition, mutex, deadline);
        }

        int condition_signal(ConditionHandle* condition) noexcept
        {
            const int context_status{ require_thread_context() };
            if (context_status != 0) {
                return context_status;
            }
            if (condition == nullptr) {
                return 0;
            }
            auto* implementation{ load_implementation<ConditionImplementation>(condition) };
            if (implementation == nullptr) {
                return 0;
            }
            raw_mutex_get(&implementation->mutex);
            Waiter* const waiter{ implementation->first };
            if (waiter != nullptr) {
                remove_waiter(implementation, waiter);
                if (tx_semaphore_put(&waiter->semaphore) != TX_SUCCESS) {
                    fatal_error("condition signal", TX_SEMAPHORE_ERROR);
                }
            }
            raw_mutex_put(&implementation->mutex);
            return 0;
        }

        int condition_broadcast(ConditionHandle* condition) noexcept
        {
            const int context_status{ require_thread_context() };
            if (context_status != 0) {
                return context_status;
            }
            if (condition == nullptr) {
                return 0;
            }
            auto* implementation{ load_implementation<ConditionImplementation>(condition) };
            if (implementation == nullptr) {
                return 0;
            }
            raw_mutex_get(&implementation->mutex);
            while (implementation->first != nullptr) {
                Waiter* const waiter{ implementation->first };
                remove_waiter(implementation, waiter);
                if (tx_semaphore_put(&waiter->semaphore) != TX_SUCCESS) {
                    fatal_error("condition broadcast", TX_SEMAPHORE_ERROR);
                }
            }
            raw_mutex_put(&implementation->mutex);
            return 0;
        }

        int once(OnceHandle* once_control, void (*function)())
        {
            const int context_status{ require_thread_context() };
            if (context_status != 0 || once_control == nullptr || function == nullptr) {
                return context_status != 0 ? context_status : EINVAL;
            }

            while (true) {
                raw_mutex_get(&initialization_mutex);
                if (once_control->state == 2U) {
                    raw_mutex_put(&initialization_mutex);
                    return 0;
                }
                if (once_control->state == 0U) {
                    once_control->state = 1U;
                    raw_mutex_put(&initialization_mutex);
                    try {
                        function();
                        raw_mutex_get(&initialization_mutex);
                        once_control->state = 2U;
                        raw_mutex_put(&initialization_mutex);
                        return 0;
                    } catch (...) {
                        raw_mutex_get(&initialization_mutex);
                        once_control->state = 0U;
                        raw_mutex_put(&initialization_mutex);
                        throw;
                    }
                }
                raw_mutex_put(&initialization_mutex);
                tx_thread_sleep(1U);
            }
        }

        int semaphore_init(SemaphoreHandle* semaphore, unsigned int value) noexcept
        {
            if (semaphore == nullptr) {
                return EINVAL;
            }
            store_implementation(semaphore, nullptr);
            semaphore->initial_count = value;
            return 0;
        }

        int semaphore_destroy(SemaphoreHandle* semaphore) noexcept
        {
            if (semaphore == nullptr) {
                return EINVAL;
            }
            auto* implementation{ load_implementation<SemaphoreImplementation>(semaphore) };
            if (implementation == nullptr) {
                return 0;
            }
            if (tx_semaphore_delete(&implementation->semaphore) != TX_SUCCESS) {
                return EBUSY;
            }
            store_implementation(semaphore, nullptr);
            delete implementation;
            return 0;
        }

        int semaphore_wait(SemaphoreHandle* semaphore) noexcept
        {
            const int context_status{ require_thread_context() };
            if (context_status != 0) {
                return context_status;
            }
            auto* implementation{ ensure_semaphore(semaphore) };
            if (implementation == nullptr) {
                return ENOMEM;
            }
            return tx_semaphore_get(&implementation->semaphore, TX_WAIT_FOREVER) == TX_SUCCESS ? 0 : EINVAL;
        }

        int semaphore_try_wait(SemaphoreHandle* semaphore) noexcept
        {
            const int context_status{ require_thread_context() };
            if (context_status != 0) {
                return context_status;
            }
            auto* implementation{ ensure_semaphore(semaphore) };
            if (implementation == nullptr) {
                return ENOMEM;
            }
            const UINT status{ tx_semaphore_get(&implementation->semaphore, TX_NO_WAIT) };
            return status == TX_SUCCESS ? 0 : EAGAIN;
        }

        int semaphore_timed_wait(SemaphoreHandle* semaphore, const TimePoint* deadline) noexcept
        {
            const int context_status{ require_thread_context() };
            if (context_status != 0) {
                return context_status;
            }
            auto* implementation{ ensure_semaphore(semaphore) };
            if (implementation == nullptr) {
                return ENOMEM;
            }
            return wait_until(
              deadline, [&](ULONG ticks) { return tx_semaphore_get(&implementation->semaphore, ticks); });
        }

        int semaphore_post(SemaphoreHandle* semaphore) noexcept
        {
            const int context_status{ require_thread_context() };
            if (context_status != 0) {
                return context_status;
            }
            auto* implementation{ ensure_semaphore(semaphore) };
            if (implementation == nullptr) {
                return ENOMEM;
            }
            return tx_semaphore_put(&implementation->semaphore) == TX_SUCCESS ? 0 : EOVERFLOW;
        }

        std::uint64_t steady_ticks() noexcept
        {
            const ULONG old_posture{ tx_interrupt_control(TX_INT_DISABLE) };
            const auto current_tick{ static_cast<std::uint32_t>(tx_time_get()) };
            const std::uint64_t result{ extend_tick_counter(tick_epoch, last_tick, current_tick) };
            static_cast<void>(tx_interrupt_control(old_posture));
            return result;
        }

        std::uint64_t extend_tick_counter(std::uint64_t& epoch,
                                          std::uint32_t& previous,
                                          std::uint32_t current) noexcept
        {
            if (current < previous) {
                epoch += (1ULL << 32U);
            }
            previous = current;
            return epoch | current;
        }

        std::int64_t steady_time_nanoseconds() noexcept { return ticks_to_nanoseconds(steady_ticks()); }

        std::int64_t system_time_nanoseconds() noexcept
        {
            const std::uint64_t current_ticks{ steady_ticks() };
            if (!system_clock_epoch_available) {
                return publish_system_time(ticks_to_nanoseconds(current_ticks));
            }

            std::int64_t elapsed{};
            if (!high_resolution_elapsed_nanoseconds(current_ticks, elapsed)) {
                elapsed = ticks_to_nanoseconds(current_ticks - system_clock_epoch_ticks);
            }
            if (elapsed == std::numeric_limits<std::int64_t>::max() ||
                system_clock_epoch_nanoseconds > std::numeric_limits<std::int64_t>::max() - elapsed) {
                return publish_system_time(std::numeric_limits<std::int64_t>::max());
            }
            return publish_system_time(system_clock_epoch_nanoseconds + elapsed);
        }

        ULONG duration_to_ticks(std::uint64_t nanoseconds) noexcept
        {
            if (nanoseconds == 0U) {
                return TX_NO_WAIT;
            }
            constexpr std::uint64_t tick_rate{ TX_TIMER_TICKS_PER_SECOND };
            const std::uint64_t whole_seconds{ nanoseconds / NANOSECONDS_PER_SECOND };
            const std::uint64_t remainder{ nanoseconds % NANOSECONDS_PER_SECOND };
            if (whole_seconds > std::numeric_limits<ULONG>::max() / tick_rate) {
                return MAX_FINITE_WAIT;
            }
            const std::uint64_t ticks{ whole_seconds * tick_rate +
                                       (remainder * tick_rate + NANOSECONDS_PER_SECOND - 1U) /
                                         NANOSECONDS_PER_SECOND };
            return static_cast<ULONG>(std::min<std::uint64_t>(ticks, MAX_FINITE_WAIT));
        }

        void sleep_for(std::uint64_t nanoseconds) noexcept
        {
            if (require_thread_context() != 0 || nanoseconds == 0U) {
                return;
            }
            const std::uint64_t whole_seconds{ nanoseconds / NANOSECONDS_PER_SECOND };
            const std::uint64_t remainder{ nanoseconds % NANOSECONDS_PER_SECOND };
            std::uint64_t ticks{ whole_seconds * TX_TIMER_TICKS_PER_SECOND +
                                 (remainder * TX_TIMER_TICKS_PER_SECOND + NANOSECONDS_PER_SECOND - 1U) /
                                   NANOSECONDS_PER_SECOND };
            while (ticks != 0U) {
                /* ThreadX timeouts are measured from the next timer boundary,
                 * so N ticks can represent slightly less than N full periods.
                 * Add one tick to satisfy sleep_for's no-early-return rule. */
                constexpr ULONG maximum_chunk{ MAX_FINITE_WAIT - 1U };
                const ULONG chunk{ static_cast<ULONG>(std::min<std::uint64_t>(ticks, maximum_chunk)) };
                if (tx_thread_sleep(chunk + 1U) != TX_SUCCESS) {
                    fatal_error("tx_thread_sleep", TX_WAIT_ERROR);
                }
                ticks -= chunk;
            }
        }

        [[noreturn]] void fatal_error(const char* operation, UINT status) noexcept
        {
            static_cast<void>(operation);
            static_cast<void>(status);
            std::terminate();
        }
    }

    UINT initialize() noexcept
    {
        if (detail::initialized) {
            return TX_SUCCESS;
        }

        detail::last_tick = static_cast<std::uint32_t>(tx_time_get());
        detail::tick_epoch = 0U;
        detail::system_clock_epoch_ticks = detail::steady_ticks();

        const auto realtime_clock{ hal::rtc::create() };
        hal::IRtc::Timestamp timestamp{};
        bool realtime_clock_available{};
        if (realtime_clock) {
            const auto result{ realtime_clock->getTime() };
            if (result) {
                timestamp = *result;
                realtime_clock_available = true;
            }
        }
        constexpr auto maximum_nanoseconds{ static_cast<std::uint64_t>(
          std::numeric_limits<std::int64_t>::max()) };
        constexpr std::uint64_t maximum_seconds{ maximum_nanoseconds / detail::NANOSECONDS_PER_SECOND };
        detail::system_clock_epoch_available =
          realtime_clock_available && timestamp.seconds_since_epoch >= 0 &&
          timestamp.nanoseconds < detail::NANOSECONDS_PER_SECOND &&
          std::cmp_less_equal(static_cast<std::uint64_t>(timestamp.seconds_since_epoch), maximum_seconds) &&
          (std::cmp_not_equal(static_cast<std::uint64_t>(timestamp.seconds_since_epoch), maximum_seconds) ||
           timestamp.nanoseconds <= maximum_nanoseconds % detail::NANOSECONDS_PER_SECOND);
        if (detail::system_clock_epoch_available) {
            detail::system_clock_epoch_nanoseconds =
              timestamp.seconds_since_epoch * static_cast<std::int64_t>(detail::NANOSECONDS_PER_SECOND) +
              timestamp.nanoseconds;
        }

        detail::high_resolution_timer = hal::timer::create(detail::HIGH_RESOLUTION_TIMER_INDEX);
        detail::high_resolution_counter_available =
          detail::high_resolution_timer && detail::high_resolution_timer->start() &&
          detail::read_high_resolution_counter(detail::high_resolution_counter_epoch);

        UINT status{ tx_mutex_create(
          &detail::initialization_mutex, detail::initialization_mutex_name, TX_INHERIT) };
        if (status != TX_SUCCESS) {
            return status;
        }
        status = detail::initialize_cxx_guard();
        if (status != TX_SUCCESS) {
            static_cast<void>(tx_mutex_delete(&detail::initialization_mutex));
            return status;
        }
        status = tx_queue_create(&detail::cleanup_queue,
                                 detail::cleanup_queue_name,
                                 TX_1_ULONG,
                                 detail::cleanup_queue_storage,
                                 sizeof(detail::cleanup_queue_storage));
        if (status != TX_SUCCESS) {
            detail::destroy_cxx_guard();
            static_cast<void>(tx_mutex_delete(&detail::initialization_mutex));
            return status;
        }
        status = tx_timer_create(&detail::rollover_timer,
                                 detail::rollover_timer_name,
                                 detail::rollover_timer_entry,
                                 0U,
                                 detail::ROLLOVER_SAMPLE_TICKS,
                                 detail::ROLLOVER_SAMPLE_TICKS,
                                 TX_AUTO_ACTIVATE);
        if (status != TX_SUCCESS) {
            static_cast<void>(tx_queue_delete(&detail::cleanup_queue));
            detail::destroy_cxx_guard();
            static_cast<void>(tx_mutex_delete(&detail::initialization_mutex));
            return status;
        }
        status = tx_thread_create(&detail::reaper_thread,
                                  detail::reaper_thread_name,
                                  detail::reaper_entry,
                                  0U,
                                  detail::reaper_stack,
                                  sizeof(detail::reaper_stack),
                                  detail::REAPER_PRIORITY,
                                  detail::REAPER_PRIORITY,
                                  TX_NO_TIME_SLICE,
                                  TX_AUTO_START);
        if (status != TX_SUCCESS) {
            static_cast<void>(tx_timer_delete(&detail::rollover_timer));
            static_cast<void>(tx_queue_delete(&detail::cleanup_queue));
            detail::destroy_cxx_guard();
            static_cast<void>(tx_mutex_delete(&detail::initialization_mutex));
            return status;
        }

        detail::initialized = true;
        return TX_SUCCESS;
    }
}

namespace
{
    void cleanup_thread_runtime(TX_THREAD* thread) noexcept
    {
        if (thread == nullptr || thread->tx_thread_runtime_cleanup_started != 0U) {
            return;
        }

        // ThreadX releases every mutex still owned by a thread immediately
        // after its entry function returns. C++ requires thread_local and
        // notify_all_at_thread_exit callbacks to run first, while an exit
        // mutex can still be owned by the departing thread.
        thread->tx_thread_runtime_cleanup_started = 1U;
#if defined(HAL_PLATFORM_STM32)
        runtime_tls_thread_exit(thread);
#endif
        runtime::detail::run_thread_specific_destructors(thread);
    }
}

extern "C" void runtime_libstdcxx_thread_entry(ULONG)
{
    TX_THREAD* const thread{ tx_thread_identify() };
    if (thread == nullptr || thread->tx_thread_runtime_entry == nullptr) {
        std::terminate();
    }

    try {
        thread->tx_thread_runtime_entry(thread->tx_thread_runtime_entry_parameter);
    } catch (...) {
        // Never unwind a C++ exception through ThreadX's C entry frame.
        std::terminate();
    }
    cleanup_thread_runtime(thread);
}

extern "C" void runtime_libstdcxx_thread_notify(TX_THREAD* thread, UINT event)
{
    if (event == TX_THREAD_EXIT && thread == tx_thread_identify()) {
        // Self-termination does not return through the wrapper, but still
        // executes in the departing thread's TLS context. External forced
        // termination cannot safely run C++ TLS/TSS callbacks in the caller;
        // those registrations are discarded when ThreadX deletes the target.
        cleanup_thread_runtime(thread);
    }
}

extern "C" void runtime_libstdcxx_thread_create(TX_THREAD* thread)
{
    if (thread == nullptr) {
        return;
    }
    for (void*& value : thread->tx_thread_runtime_tls_values) {
        value = nullptr;
    }
    for (unsigned int& generation : thread->tx_thread_runtime_tls_generations) {
        generation = 0U;
    }
    thread->tx_thread_runtime_entry = thread->tx_thread_entry;
    thread->tx_thread_runtime_entry_parameter = thread->tx_thread_entry_parameter;
    thread->tx_thread_runtime_cleanup_started = 0U;
    thread->tx_thread_entry = runtime_libstdcxx_thread_entry;
    thread->tx_thread_entry_parameter = 0U;
}

extern "C" void runtime_libstdcxx_thread_started(TX_THREAD* thread)
{
    // This hook also runs after tx_thread_reset(). Do not retain TSS values
    // from the previous execution, including keys without destructors.
    for (void*& value : thread->tx_thread_runtime_tls_values) {
        value = nullptr;
    }
    for (unsigned int& generation : thread->tx_thread_runtime_tls_generations) {
        generation = 0U;
    }
    thread->tx_thread_runtime_cleanup_started = 0U;
    if (tx_thread_entry_exit_notify(thread, runtime_libstdcxx_thread_notify) != TX_SUCCESS) {
        std::terminate();
    }
}

extern "C" void runtime_libstdcxx_initialize(void)
{
    if (runtime::initialize() != TX_SUCCESS) {
        std::terminate();
    }
}

// NOLINTEND(bugprone-easily-swappable-parameters,cppcoreguidelines-avoid-c-arrays,cppcoreguidelines-avoid-magic-numbers,cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-macro-usage,cppcoreguidelines-missing-std-forward,cppcoreguidelines-owning-memory,cppcoreguidelines-pro-bounds-array-to-pointer-decay,cppcoreguidelines-pro-type-const-cast,cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-type-vararg,modernize-avoid-c-arrays,readability-magic-numbers,readability-math-missing-parentheses,readability-named-parameter)
