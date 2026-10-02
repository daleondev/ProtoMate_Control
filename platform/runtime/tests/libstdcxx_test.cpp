#include <gtest/gtest.h>
#include <tx_api.h>

#include "libstdcxx/backend.hpp"
#include "runtime/thread.hpp"

#if defined(HAL_PLATFORM_LINUX)
#include "tx_thread_stack_info.hpp"
#endif

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <semaphore>
#include <shared_mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

using namespace std::chrono_literals;

namespace
{
    constexpr std::uint64_t WIDE_ATOMIC_INITIAL{ 0x1'0000'0000ULL };
    constexpr int PROMISE_VALUE{ 42 };
    constexpr int ASYNC_VALUE{ 43 };
    constexpr int TASK_VALUE{ 44 };
    constexpr int EXIT_PROMISE_VALUE{ 45 };
    constexpr int DEFERRED_VALUE{ 46 };
    constexpr int SHARED_VALUE{ 47 };
    constexpr int DELAYED_VALUE{ 48 };
    constexpr std::uint32_t STATIC_VALUE{ 0x51A71C42U };

    struct AtomicRecord
    {
        std::uint32_t first;
        std::uint32_t second;
        std::uint32_t third;

        constexpr bool operator==(const AtomicRecord&) const = default;
    };

    constexpr AtomicRecord FIRST_RECORD{ .first = 1U, .second = 2U, .third = 3U };
    constexpr AtomicRecord SECOND_RECORD{ .first = 4U, .second = 5U, .third = 6U };
    constexpr AtomicRecord THIRD_RECORD{ .first = 7U, .second = 8U, .third = 9U };

    std::atomic_uint static_access_count{};
    std::atomic_uint static_constructor_count{};
    std::atomic_bool static_constructor_synchronized{};
    std::atomic_uint retrying_static_attempts{};
    std::atomic_uint thread_local_destructor_count{};

    class ThreadLocalState
    {
      public:
        ~ThreadLocalState() { thread_local_destructor_count.fetch_add(1U); }

        int value{};
    };

    thread_local ThreadLocalState thread_local_state;

#if defined(HAL_PLATFORM_STM32)
    constexpr int STARTUP_TLS_INITIAL_VALUE{ 0x1357 };
    constexpr int STARTUP_TLS_UPDATED_VALUE{ 0x2468 };

    class alignas(16) StartupThreadLocalState
    {
      public:
        StartupThreadLocalState()
          : self{ this }
          , value{ STARTUP_TLS_INITIAL_VALUE }
        {
        }

        ~StartupThreadLocalState() { self = nullptr; }

        StartupThreadLocalState* self;
        int value;
    };

    thread_local StartupThreadLocalState startup_thread_local_state;
    StartupThreadLocalState* startup_thread_local_address{};

    class StartupThreadLocalAccess
    {
      public:
        StartupThreadLocalAccess() { startup_thread_local_address = &startup_thread_local_state; }
    };

    StartupThreadLocalAccess startup_thread_local_access;

    struct ThreadExitTlsProbe
    {
        ~ThreadExitTlsProbe()
        {
            if (mutex != nullptr) {
                const bool acquired{ mutex->try_lock() };
                unexpectedly_unlocked->store(acquired, std::memory_order_release);
                if (acquired) {
                    mutex->unlock();
                }
                checked->store(true, std::memory_order_release);
            }
        }

        std::mutex* mutex{};
        std::atomic_bool* checked{};
        std::atomic_bool* unexpectedly_unlocked{};
    };

    thread_local ThreadExitTlsProbe thread_exit_tls_probe;
#endif

    class ThreadsafeStatic
    {
      public:
        ThreadsafeStatic()
        {
            static_constructor_count.fetch_add(1U, std::memory_order_relaxed);
            const auto deadline{ std::chrono::steady_clock::now() + 100ms };
            while (static_access_count.load(std::memory_order_acquire) < 2U &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            static_constructor_synchronized.store(static_access_count.load(std::memory_order_acquire) == 2U,
                                                  std::memory_order_release);
        }

        [[nodiscard]] auto value() const noexcept -> std::uint32_t { return STATIC_VALUE; }
    };

    auto threadsafe_static() -> ThreadsafeStatic&
    {
        static ThreadsafeStatic instance;
        return instance;
    }

    auto retrying_static() -> int&
    {
        static int value{ [] {
            if (retrying_static_attempts.fetch_add(1U) == 0U) {
                throw std::runtime_error{ "retry static initialization" };
            }
            return PROMISE_VALUE;
        }() };
        return value;
    }
}

TEST(RuntimeLibstdcxx, MutexesAndConditionVariables)
{
    std::mutex mutex;
    std::condition_variable condition;
    bool ready{};
    std::thread condition_thread{ [&] {
        {
            std::lock_guard lock{ mutex };
            ready = true;
        }
        condition.notify_one();
    } };
    {
        std::unique_lock lock{ mutex };
        condition.wait(lock, [&] { return ready; });
    }
    condition_thread.join();

    {
        std::unique_lock lock{ mutex };
        EXPECT_EQ(condition.wait_for(lock, 1ms), std::cv_status::timeout);
    }

    std::recursive_mutex recursive_mutex;
    recursive_mutex.lock();
    recursive_mutex.lock();
    recursive_mutex.unlock();
    recursive_mutex.unlock();

    std::timed_mutex timed_mutex;
    timed_mutex.lock();
    bool timed_out{};
    std::thread timed_thread{ [&] { timed_out = !timed_mutex.try_lock_for(1ms); } };
    timed_thread.join();
    timed_mutex.unlock();
    EXPECT_TRUE(timed_out);

    std::shared_mutex shared_mutex;
    shared_mutex.lock_shared();
    shared_mutex.unlock_shared();
    shared_mutex.lock();
    shared_mutex.unlock();

    std::shared_timed_mutex shared_timed_mutex;
    shared_timed_mutex.lock();
    bool shared_timed_out{};
    std::thread shared_timed_thread{ [&] {
        shared_timed_out = !shared_timed_mutex.try_lock_shared_for(1ms);
    } };
    shared_timed_thread.join();
    shared_timed_mutex.unlock();
    EXPECT_TRUE(shared_timed_out);
}

TEST(RuntimeLibstdcxx, ConcurrentFirstUsePublishesOneNativeObject)
{
    std::mutex mutex;
    std::counting_semaphore<2> start{ 0 };
    std::atomic_int protected_count{};

    auto worker = [&] {
        start.acquire();
        for (int iteration{}; iteration < 64; ++iteration) {
            std::lock_guard lock{ mutex };
            protected_count.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    };

    std::thread first{ worker };
    std::thread second{ worker };
    start.release(2);
    first.join();
    second.join();

    EXPECT_EQ(protected_count.load(std::memory_order_relaxed), 128);

    std::binary_semaphore semaphore{ 0 };
    std::latch semaphore_start{ 2 };
    std::thread acquirer{ [&] {
        semaphore_start.count_down();
        semaphore_start.wait();
        semaphore.acquire();
    } };
    std::thread releaser{ [&] {
        semaphore_start.count_down();
        semaphore_start.wait();
        semaphore.release();
    } };
    acquirer.join();
    releaser.join();
}

TEST(RuntimeLibstdcxx, RandomDeviceUsesHardwareEntropyProvider)
{
    std::random_device default_device;
    std::random_device hardware_device{ "hardware" };

    EXPECT_EQ(default_device.entropy(), std::numeric_limits<std::random_device::result_type>::digits);
    static_cast<void>(default_device());
    static_cast<void>(hardware_device());

    EXPECT_THROW(static_cast<void>(std::random_device{ "mt19937" }), std::runtime_error);
}

TEST(RuntimeLibstdcxx, AbortedConditionWaitIsUnlinked)
{
    runtime::detail::MutexHandle mutex{};
    runtime::detail::ConditionHandle condition{};
    runtime::detail::mutex_init(&mutex);
    runtime::detail::condition_init(&condition);

    std::atomic_bool entered{};
    std::atomic_int wait_status{};

    std::thread waiter{ [&] {
        if (runtime::detail::mutex_lock(&mutex) != 0) {
            wait_status.store(EINVAL, std::memory_order_release);
            return;
        }
        entered.store(true, std::memory_order_release);
        wait_status.store(runtime::detail::condition_wait(&condition, &mutex), std::memory_order_release);
        static_cast<void>(runtime::detail::mutex_unlock(&mutex));
    } };

    // Let the lower-priority std::thread finish entering its semaphore wait
    // before attempting to abort that wait.
    UINT original_priority{};
    const UINT lower_status{ tx_thread_priority_change(tx_thread_identify(), 17U, &original_priority) };
    UINT restore_status{ TX_SUCCESS };
    if (lower_status == TX_SUCCESS) {
        UINT discarded_priority{};
        restore_status =
          tx_thread_priority_change(tx_thread_identify(), original_priority, &discarded_priority);
    }

    UINT state{};
    const UINT info_status{ tx_thread_info_get(
      waiter.native_handle(), nullptr, &state, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) };
    const bool suspended{ info_status == TX_SUCCESS && state == TX_SEMAPHORE_SUSP };

    const UINT abort_status{ suspended ? tx_thread_wait_abort(waiter.native_handle()) : TX_WAIT_ABORT_ERROR };
    if (abort_status != TX_SUCCESS) {
        static_cast<void>(runtime::detail::condition_signal(&condition));
    }
    waiter.join();

    EXPECT_EQ(info_status, TX_SUCCESS);
    EXPECT_EQ(lower_status, TX_SUCCESS);
    EXPECT_EQ(restore_status, TX_SUCCESS);
    EXPECT_EQ(state, TX_SEMAPHORE_SUSP);
    EXPECT_TRUE(suspended);
    EXPECT_EQ(abort_status, TX_SUCCESS);
    EXPECT_TRUE(entered.load(std::memory_order_acquire));
    EXPECT_EQ(wait_status.load(std::memory_order_acquire), EINVAL);

    // This used to address the deleted semaphore through a waiter that was
    // still linked after TX_WAIT_ABORTED.
    EXPECT_EQ(runtime::detail::condition_signal(&condition), 0);
    EXPECT_EQ(runtime::detail::condition_destroy(&condition), 0);
    EXPECT_EQ(runtime::detail::mutex_destroy(&mutex), 0);
}

TEST(RuntimeLibstdcxx, RecursiveConditionWaitAbi)
{
    runtime::detail::RecursiveMutexHandle mutex{};
    runtime::detail::ConditionHandle condition{};
    runtime::detail::recursive_mutex_init(&mutex);
    runtime::detail::condition_init(&condition);

    ASSERT_EQ(runtime::detail::recursive_mutex_lock(&mutex), 0);
    ASSERT_EQ(runtime::detail::recursive_mutex_lock(&mutex), 0);
    EXPECT_EQ(runtime::detail::condition_wait_recursive(&condition, &mutex), EINVAL);
    ASSERT_EQ(runtime::detail::recursive_mutex_unlock(&mutex), 0);
    ASSERT_EQ(runtime::detail::recursive_mutex_unlock(&mutex), 0);

    ASSERT_EQ(runtime::detail::recursive_mutex_lock(&mutex), 0);
    std::thread notifier{ [&] {
        EXPECT_EQ(runtime::detail::recursive_mutex_lock(&mutex), 0);
        EXPECT_EQ(runtime::detail::condition_signal(&condition), 0);
        EXPECT_EQ(runtime::detail::recursive_mutex_unlock(&mutex), 0);
    } };
    EXPECT_EQ(runtime::detail::condition_wait_recursive(&condition, &mutex), 0);
    EXPECT_EQ(runtime::detail::recursive_mutex_unlock(&mutex), 0);
    notifier.join();

    EXPECT_EQ(runtime::detail::condition_destroy(&condition), 0);
    EXPECT_EQ(runtime::detail::recursive_mutex_destroy(&mutex), 0);
}

TEST(RuntimeLibstdcxx, Semaphores)
{
    std::binary_semaphore binary_semaphore{ 0 };
    std::thread binary_thread{ [&] { binary_semaphore.release(); } };
    binary_semaphore.acquire();
    binary_thread.join();

    std::counting_semaphore<4> counting_semaphore{ 0 };
    counting_semaphore.release(2);
    counting_semaphore.acquire();
    counting_semaphore.acquire();
    EXPECT_FALSE(counting_semaphore.try_acquire());
    EXPECT_FALSE(counting_semaphore.try_acquire_for(1ms));
}

TEST(RuntimeLibstdcxx, Atomics)
{
    std::atomic<int> atomic_value{};
    std::thread atomic_thread{ [&] {
        atomic_value.wait(0);
        atomic_value.fetch_add(1);
    } };
    atomic_value.store(1);
    atomic_value.notify_one();
    atomic_thread.join();
    EXPECT_EQ(atomic_value.load(), 2);

    std::atomic<std::uint64_t> wide_atomic{ WIDE_ATOMIC_INITIAL };
    EXPECT_EQ(wide_atomic.fetch_add(1U), WIDE_ATOMIC_INITIAL);
    EXPECT_EQ(wide_atomic.load(), WIDE_ATOMIC_INITIAL + 1U);
    std::uint64_t wide_expected{ WIDE_ATOMIC_INITIAL + 1U };
    EXPECT_TRUE(wide_atomic.compare_exchange_strong(wide_expected, WIDE_ATOMIC_INITIAL + 2U));
    EXPECT_EQ(wide_atomic.exchange(WIDE_ATOMIC_INITIAL), WIDE_ATOMIC_INITIAL + 2U);
    wide_atomic.store(WIDE_ATOMIC_INITIAL + 1U);
    EXPECT_EQ(wide_atomic.load(), WIDE_ATOMIC_INITIAL + 1U);

    std::atomic<AtomicRecord> record{ FIRST_RECORD };
    EXPECT_EQ(record.load(), FIRST_RECORD);
    EXPECT_EQ(record.exchange(SECOND_RECORD), FIRST_RECORD);
    AtomicRecord expected{ SECOND_RECORD };
    EXPECT_TRUE(record.compare_exchange_strong(expected, THIRD_RECORD));
    EXPECT_EQ(record.load(), THIRD_RECORD);
    record.store(FIRST_RECORD);
    EXPECT_EQ(record.load(), FIRST_RECORD);

    std::atomic_flag flag;
    flag.test_and_set();
    std::thread flag_thread{ [&] {
        flag.wait(true);
        atomic_value.fetch_add(1);
    } };
    flag.clear();
    flag.notify_one();
    flag_thread.join();
    EXPECT_EQ(atomic_value.load(), 3);
}

TEST(RuntimeLibstdcxx, FuturesPromisesAndTasks)
{
    std::promise<int> promise;
    auto promised_value{ promise.get_future() };
    std::thread promise_thread{ [&] { promise.set_value(PROMISE_VALUE); } };
    EXPECT_EQ(promised_value.get(), PROMISE_VALUE);
    promise_thread.join();

    std::promise<int> delayed_promise;
    auto delayed_value{ delayed_promise.get_future() };
    EXPECT_EQ(delayed_value.wait_for(1ms), std::future_status::timeout);
    delayed_promise.set_value(DELAYED_VALUE);
    EXPECT_EQ(delayed_value.wait_for(10ms), std::future_status::ready);
    EXPECT_EQ(delayed_value.get(), DELAYED_VALUE);

    std::promise<int> exit_promise;
    auto exit_value{ exit_promise.get_future() };
    std::thread exit_promise_thread{ [promise_at_exit = std::move(exit_promise)]() mutable {
        promise_at_exit.set_value_at_thread_exit(EXIT_PROMISE_VALUE);
    } };
    exit_promise_thread.join();
    EXPECT_EQ(exit_value.get(), EXIT_PROMISE_VALUE);

    auto asynchronous_value{ std::async(std::launch::async, [] { return ASYNC_VALUE; }) };
    auto deferred_value{ std::async(std::launch::deferred, [] { return DEFERRED_VALUE; }) };
    EXPECT_EQ(asynchronous_value.get(), ASYNC_VALUE);
    EXPECT_EQ(deferred_value.wait_for(0ms), std::future_status::deferred);
    EXPECT_EQ(deferred_value.get(), DEFERRED_VALUE);

    std::packaged_task<int()> task{ [] { return TASK_VALUE; } };
    auto task_value{ task.get_future() };
    std::thread task_thread{ std::move(task) };
    task_thread.join();
    EXPECT_EQ(task_value.get(), TASK_VALUE);

    std::promise<int> shared_promise;
    std::shared_future<int> shared_value{ shared_promise.get_future() };
    shared_promise.set_value(SHARED_VALUE);
    EXPECT_EQ(shared_value.get(), SHARED_VALUE);
    EXPECT_EQ(shared_value.get(), SHARED_VALUE);

    std::promise<void> detached_promise;
    auto detached_completion{ detached_promise.get_future() };
    std::thread detached_thread{ [promise = std::move(detached_promise)]() mutable { promise.set_value(); } };
    detached_thread.detach();
    EXPECT_EQ(detached_completion.wait_for(100ms), std::future_status::ready);
}

TEST(RuntimeLibstdcxx, StopTokensAndJthreads)
{
    std::atomic<bool> stopped{};
    std::jthread interruptible_thread{ [&](const std::stop_token& token) {
        while (!token.stop_requested()) {
            std::this_thread::yield();
        }
        stopped.store(true);
    } };
    EXPECT_TRUE(interruptible_thread.request_stop());
    interruptible_thread.join();
    EXPECT_TRUE(stopped.load());

    std::condition_variable_any interruptible_condition;
    std::mutex interruptible_mutex;
    bool wait_was_stopped{};
    std::jthread condition_any_thread{ [&](std::stop_token token) {
        std::unique_lock lock{ interruptible_mutex };
        wait_was_stopped = !interruptible_condition.wait(lock, std::move(token), [] { return false; });
    } };
    EXPECT_TRUE(condition_any_thread.request_stop());
    condition_any_thread.join();
    EXPECT_TRUE(wait_was_stopped);
}

TEST(RuntimeLibstdcxx, BarriersAndLatches)
{
    std::atomic<int> barrier_count{};
    std::barrier synchronization_point{ 3 };
    std::jthread barrier_thread_1{ [&] {
        barrier_count.fetch_add(1);
        synchronization_point.arrive_and_wait();
    } };
    std::jthread barrier_thread_2{ [&] {
        barrier_count.fetch_add(1);
        synchronization_point.arrive_and_wait();
    } };
    synchronization_point.arrive_and_wait();
    barrier_thread_1.join();
    barrier_thread_2.join();
    EXPECT_EQ(barrier_count.load(), 2);

    std::latch completion{ 2 };
    std::jthread latch_thread_1{ [&] { completion.count_down(); } };
    std::jthread latch_thread_2{ [&] { completion.count_down(); } };
    completion.wait();
}

TEST(RuntimeLibstdcxx, ThreadExitAndCallOnce)
{
    std::mutex exit_mutex;
    std::condition_variable exit_condition;
    bool exiting{};
    std::thread notifying_thread{ [&] {
        std::unique_lock lock{ exit_mutex };
        exiting = true;
        std::notify_all_at_thread_exit(exit_condition, std::move(lock));
    } };
    {
        std::unique_lock lock{ exit_mutex };
        exit_condition.wait(lock, [&] { return exiting; });
    }
    notifying_thread.join();

    std::once_flag once;
    int once_count{};
    std::thread once_thread_1{ [&] { std::call_once(once, [&] { ++once_count; }); } };
    std::thread once_thread_2{ [&] { std::call_once(once, [&] { ++once_count; }); } };
    once_thread_1.join();
    once_thread_2.join();
    EXPECT_EQ(once_count, 1);

    std::once_flag retry_once;
    int retry_count{};
    EXPECT_THROW(std::call_once(retry_once,
                                [&] {
        ++retry_count;
        throw std::runtime_error{ "retry" };
    }),
                 std::runtime_error);
    std::call_once(retry_once, [&] { ++retry_count; });
    EXPECT_EQ(retry_count, 2);

    constexpr std::size_t reusable_once_flag_count{ 256U };
    std::size_t completed{};
    for (std::size_t index{}; index < reusable_once_flag_count; ++index) {
        std::once_flag reusable_once;
        std::call_once(reusable_once, [&] { ++completed; });
    }
    EXPECT_EQ(completed, reusable_once_flag_count);

    std::once_flag outer_once;
    std::once_flag inner_once;
    int nested_count{};
    std::call_once(outer_once, [&] {
        std::call_once(inner_once, [&] { ++nested_count; });
        ++nested_count;
    });
    std::call_once(inner_once, [&] { ++nested_count; });
    EXPECT_EQ(nested_count, 2);
}

TEST(RuntimeLibstdcxx, ThreadExitDestructorCanJoinAnotherThread)
{
    struct ExitState
    {
        bool child_ran{};
        bool destructor_finished{};
    } state;
    runtime::detail::KeyHandle key{};
    ASSERT_EQ(runtime::detail::key_create(&key, [](void* pointer) {
        auto& current{ *static_cast<ExitState*>(pointer) };
        std::thread child{ [&] { current.child_ran = true; } };
        child.join();
        current.destructor_finished = true;
    }), 0);

    int key_status{};
    std::thread worker{ [&] { key_status = runtime::detail::key_set(key, &state); } };
    worker.join();

    EXPECT_EQ(key_status, 0);
    EXPECT_TRUE(state.child_ran);
    EXPECT_TRUE(state.destructor_finished);
    EXPECT_EQ(runtime::detail::key_delete(key), 0);
}

#if defined(HAL_PLATFORM_STM32)
TEST(RuntimeLibstdcxx, StartupThreadLocalObjectKeepsItsIdentity)
{
    ASSERT_NE(startup_thread_local_address, nullptr);
    EXPECT_EQ(&startup_thread_local_state, startup_thread_local_address);
    EXPECT_EQ(startup_thread_local_state.self, startup_thread_local_address);
    EXPECT_EQ(startup_thread_local_state.value, STARTUP_TLS_INITIAL_VALUE);

    startup_thread_local_state.value = STARTUP_TLS_UPDATED_VALUE;
    EXPECT_EQ(startup_thread_local_address->value, STARTUP_TLS_UPDATED_VALUE);
}

TEST(RuntimeLibstdcxx, AtThreadExitKeepsMutexLockedThroughTlsDestruction)
{
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic_bool destructor_checked{};
    std::atomic_bool mutex_was_unexpectedly_unlocked{};
    bool exiting{};

    std::thread worker{ [&] {
        std::unique_lock lock{ mutex };
        thread_exit_tls_probe.mutex = &mutex;
        thread_exit_tls_probe.checked = &destructor_checked;
        thread_exit_tls_probe.unexpectedly_unlocked = &mutex_was_unexpectedly_unlocked;
        exiting = true;
        std::notify_all_at_thread_exit(condition, std::move(lock));
    } };

    {
        std::unique_lock lock{ mutex };
        condition.wait(lock, [&] { return exiting; });
    }
    worker.join();

    EXPECT_TRUE(destructor_checked.load(std::memory_order_acquire));
    EXPECT_FALSE(mutex_was_unexpectedly_unlocked.load(std::memory_order_acquire));
}
#endif

#if defined(HAL_PLATFORM_LINUX)
TEST(RuntimeLibstdcxx, JoinedThreadsReleaseLinuxHostStackMetadata)
{
    const std::size_t baseline{ _tx_linux_thread_stack_live_count() };
    for (std::size_t iteration{}; iteration < 64U; ++iteration) {
        std::thread worker{ [] {} };
        worker.join();
        EXPECT_EQ(_tx_linux_thread_stack_live_count(), baseline);
    }
}
#endif

TEST(RuntimeLibstdcxx, ThreadFactorySelectsStackSizePriorityAndForwardsArguments)
{
    constexpr std::size_t stack_alignment{ 8U };
    constexpr std::size_t requested_stack_size{ 8192U + 13U };
    constexpr std::size_t normalized_stack_size{ (requested_stack_size + stack_alignment - 1U) &
                                                 ~(stack_alignment - 1U) };
    constexpr std::int32_t requested_priority{ 17 };
    ULONG observed_stack_size{};
    UINT observed_priority{};
    int result{};
    auto right{ std::make_unique<int>(23) };

    auto worker =
      runtime::thread::create({ .priority = requested_priority, .stack_size = requested_stack_size },
                              [&](int left, auto right_argument) {
        TX_THREAD* const current{ tx_thread_identify() };
        if (current != nullptr) {
            observed_stack_size = current->tx_thread_stack_size;
            observed_priority = current->tx_thread_priority;
        }
        result = left + *right_argument;
    },
                              19,
                              std::move(right));
    worker.join();

    EXPECT_EQ(result, 42);
    EXPECT_EQ(right, nullptr);
    // Stack checking reserves one aligned word at creation time.
    EXPECT_EQ(observed_stack_size, static_cast<ULONG>(normalized_stack_size - sizeof(ULONG)));
    EXPECT_EQ(observed_priority, static_cast<UINT>(requested_priority));
}

TEST(RuntimeLibstdcxx, ThreadFactoryBoundsCustomNames)
{
    // The name need not be NUL-terminated, including at the conversion buffer's
    // exact capacity. Bytes beyond the view must never be copied into the name.
    constexpr std::array name_storage{ 'i', 'o', '_', 'w', 'o', 'r', 'k', 'e', 'r', 'X' };
    const std::array<std::pair<std::string_view, std::string_view>, 4U> cases{{
        { std::string_view{ name_storage.data(), name_storage.size() - 1U }, "IoWorker Thread" },
        { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" },
        { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "Unknown Thread" },
        { "___", " Thread" },
    }};

    for (const auto& [requested, expected] : cases) {
        std::string observed;
        auto worker = runtime::thread::create({ .name = requested }, [&] {
            observed = tx_thread_identify()->tx_thread_name;
        });
        worker.join();
        EXPECT_EQ(observed, expected);
    }
}

TEST(RuntimeLibstdcxx, ZeroThreadAttributeUsesConfiguredDefault)
{
    ULONG ordinary_stack_size{};
    ULONG attributed_stack_size{};

    std::thread ordinary{ [&] { ordinary_stack_size = tx_thread_identify()->tx_thread_stack_size; } };
    ordinary.join();

    auto attributed = runtime::thread::create(
      {}, [&] { attributed_stack_size = tx_thread_identify()->tx_thread_stack_size; });
    attributed.join();

    EXPECT_NE(ordinary_stack_size, 0U);
    EXPECT_EQ(attributed_stack_size, ordinary_stack_size);
}

TEST(RuntimeLibstdcxx, ThreadFactoryAttributesDoNotLeak)
{
    constexpr std::size_t requested_stack_size{ 8192U };
    ULONG default_stack_size{};
    ULONG attributed_stack_size{};
    ULONG following_stack_size{};

    std::thread baseline{ [&] { default_stack_size = tx_thread_identify()->tx_thread_stack_size; } };
    baseline.join();

    auto attributed = runtime::thread::create({ .stack_size = requested_stack_size }, [&] {
        attributed_stack_size = tx_thread_identify()->tx_thread_stack_size;
    });
    attributed.join();

    std::thread following{ [&] { following_stack_size = tx_thread_identify()->tx_thread_stack_size; } };
    following.join();

    EXPECT_EQ(attributed_stack_size, static_cast<ULONG>(requested_stack_size - sizeof(ULONG)));
    EXPECT_NE(default_stack_size, attributed_stack_size);
    EXPECT_EQ(following_stack_size, default_stack_size);
}

TEST(RuntimeLibstdcxx, ThreadFactoriesAreIsolatedByCreatingThread)
{
    constexpr std::array<std::size_t, 2U> requested_stack_sizes{ 8192U, 12288U };
    std::array<ULONG, requested_stack_sizes.size()> observed_stack_sizes{};
    std::barrier start{ static_cast<std::ptrdiff_t>(requested_stack_sizes.size()) };
    std::array<std::thread, requested_stack_sizes.size()> creators;

    for (std::size_t index{}; index < creators.size(); ++index) {
        creators[index] = std::thread{ [&, index] {
            start.arrive_and_wait();
            auto child = runtime::thread::create({ .stack_size = requested_stack_sizes[index] }, [&] {
                observed_stack_sizes[index] = tx_thread_identify()->tx_thread_stack_size;
            });
            child.join();
        } };
    }
    for (auto& creator : creators) {
        creator.join();
    }

    for (std::size_t index{}; index < observed_stack_sizes.size(); ++index) {
        EXPECT_EQ(observed_stack_sizes[index],
                  static_cast<ULONG>(requested_stack_sizes[index] - sizeof(ULONG)));
    }
}

TEST(RuntimeLibstdcxx, ThreadFactoryRejectsInvalidAttributes)
{
    const auto creation_error = [](runtime::thread::Attributes attributes) {
        try {
            auto unexpected = runtime::thread::create(attributes, [] {});
            unexpected.join();
            return std::error_code{};
        } catch (const std::system_error& error) {
            return error.code();
        }
    };

    const auto invalid_argument{ std::make_error_code(std::errc::invalid_argument) };
    EXPECT_EQ(creation_error({ .stack_size = TX_MINIMUM_STACK - 1U }), invalid_argument);
    EXPECT_EQ(creation_error({ .stack_size = std::numeric_limits<std::size_t>::max() }), invalid_argument);
    EXPECT_EQ(creation_error({ .priority = -2 }), invalid_argument);
    EXPECT_EQ(creation_error({ .priority = static_cast<std::int32_t>(TX_MAX_PRIORITIES) }), invalid_argument);
}

TEST(RuntimeLibstdcxx, ThreadFactoryRestoresPreviouslyPublishedAttributes)
{
    constexpr std::size_t factory_stack_size{ 8192U };
    constexpr std::size_t published_stack_size{ 12288U };
    ULONG factory_observed_stack_size{};
    ULONG published_observed_stack_size{};

    runtime::thread::publish_attributes({ .stack_size = published_stack_size });
    auto factory_thread = runtime::thread::create({ .stack_size = factory_stack_size }, [&] {
        factory_observed_stack_size = tx_thread_identify()->tx_thread_stack_size;
    });
    factory_thread.join();

    std::thread published_thread{ [&] {
        published_observed_stack_size = tx_thread_identify()->tx_thread_stack_size;
    } };
    published_thread.join();

    EXPECT_EQ(factory_observed_stack_size, static_cast<ULONG>(factory_stack_size - sizeof(ULONG)));
    EXPECT_EQ(published_observed_stack_size, static_cast<ULONG>(published_stack_size - sizeof(ULONG)));
}

TEST(RuntimeLibstdcxx, ThreadFactoryPreparationFailureDoesNotDisturbPendingAttributes)
{
    class ThrowWhenCopied
    {
      public:
        ThrowWhenCopied() = default;
        ThrowWhenCopied(const ThrowWhenCopied&) { throw std::runtime_error{ "copy failed" }; }
        ThrowWhenCopied(ThrowWhenCopied&&) = default;
        void operator()() const {}
    };

    constexpr std::size_t published_stack_size{ 8192U };
    ULONG observed_stack_size{};
    ThrowWhenCopied callable;
    bool threw{};

    runtime::thread::publish_attributes({ .stack_size = published_stack_size });
    try {
        auto unexpected = runtime::thread::create({ .stack_size = 12288U }, callable);
        unexpected.join();
    } catch (const std::runtime_error&) {
        threw = true;
    }

    std::thread published_thread{ [&] { observed_stack_size = tx_thread_identify()->tx_thread_stack_size; } };
    published_thread.join();

    EXPECT_TRUE(threw);
    EXPECT_EQ(observed_stack_size, static_cast<ULONG>(published_stack_size - sizeof(ULONG)));
}

TEST(RuntimeLibstdcxx, ThreadFactoryCreationFailureReleasesInvocationAndRestoresAttributes)
{
    constexpr std::size_t published_stack_size{ 8192U };
    ULONG observed_stack_size{};
    auto lifetime{ std::make_shared<int>(42) };
    const std::weak_ptr<int> weak_lifetime{ lifetime };

    runtime::thread::publish_attributes({ .stack_size = published_stack_size });
    try {
        auto unexpected = runtime::thread::create({ .stack_size = TX_MINIMUM_STACK - 1U }, [lifetime] {});
        unexpected.join();
        ADD_FAILURE() << "thread creation unexpectedly succeeded";
    } catch (const std::system_error& error) {
        EXPECT_EQ(error.code(), std::make_error_code(std::errc::invalid_argument));
    }
    lifetime.reset();

    std::thread published_thread{ [&] { observed_stack_size = tx_thread_identify()->tx_thread_stack_size; } };
    published_thread.join();

    EXPECT_TRUE(weak_lifetime.expired());
    EXPECT_EQ(observed_stack_size, static_cast<ULONG>(published_stack_size - sizeof(ULONG)));
}

TEST(RuntimeLibstdcxx, JthreadFactoryPreservesBothInvocationForms)
{
    constexpr std::size_t requested_stack_size{ 8192U };
    ULONG observed_stack_size{};
    bool stop_possible{};
    int result{};
    int plain_result{};

    auto worker = runtime::thread::create_jthread(
      { .stack_size = requested_stack_size }, [&](std::stop_token token, int value) {
        observed_stack_size = tx_thread_identify()->tx_thread_stack_size;
        stop_possible = token.stop_possible();
        result = value;
    }, 42);
    worker.join();

    auto plain_worker = runtime::thread::create_jthread({}, [&](int value) { plain_result = value; }, 43);
    plain_worker.join();

    EXPECT_EQ(observed_stack_size, static_cast<ULONG>(requested_stack_size - sizeof(ULONG)));
    EXPECT_TRUE(stop_possible);
    EXPECT_EQ(result, 42);
    EXPECT_EQ(plain_result, 43);
}

TEST(RuntimeLibstdcxx, ContendedStaticInitialization)
{
    std::array<ThreadsafeStatic*, 2U> instances{};
    std::array<std::thread, 2U> workers;

    for (std::size_t index{}; index < workers.size(); ++index) {
        workers[index] = std::thread{ [&, index] {
            static_access_count.fetch_add(1U, std::memory_order_release);
            instances[index] = &threadsafe_static();
        } };
    }
    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_TRUE(static_constructor_synchronized.load(std::memory_order_acquire));
    EXPECT_EQ(static_constructor_count.load(std::memory_order_acquire), 1U);
    ASSERT_NE(instances[0], nullptr);
    EXPECT_EQ(instances[0], instances[1]);
    EXPECT_EQ(instances[0]->value(), STATIC_VALUE);
}

TEST(RuntimeLibstdcxx, StaticInitializerCanJoinThreadInitializingAnotherStatic)
{
    static const int outer{ [] {
        int result{};
        std::thread child{ [&] {
            static const int inner{ [] { return tx_thread_identify() != nullptr ? 42 : 0; }() };
            result = inner;
        } };
        child.join();
        return result;
    }() };

    EXPECT_EQ(outer, 42);
}

TEST(RuntimeLibstdcxx, FailedStaticInitializationCanBeRetried)
{
    if (retrying_static_attempts.load() == 0U) {
        EXPECT_THROW(static_cast<void>(retrying_static()), std::runtime_error);
    }
    EXPECT_EQ(retrying_static(), PROMISE_VALUE);
    EXPECT_EQ(retrying_static_attempts.load(), 2U);
}

TEST(RuntimeLibstdcxx, ThreadLocalStorageAndConcurrentExceptions)
{
    constexpr std::size_t worker_count{ 2U };
    const auto destructor_count_before{ thread_local_destructor_count.load() };
    thread_local_state.value = PROMISE_VALUE;
    ThreadLocalState* const main_state{ &thread_local_state };

    std::barrier inside_catch{ static_cast<std::ptrdiff_t>(worker_count + 1U) };
    std::array<ThreadLocalState*, worker_count> states{};
    std::array<bool, worker_count> passed{};
    std::array<std::thread, worker_count> workers;

    for (std::size_t index{}; index < workers.size(); ++index) {
        workers[index] = std::thread{ [&, index] {
            states[index] = &thread_local_state;
            const int expected{ static_cast<int>(index) + 1 };
            const bool initially_zero{ thread_local_state.value == 0 };
            thread_local_state.value = expected;

            try {
                throw expected;
            } catch (...) {
                const std::exception_ptr exception{ std::current_exception() };
                inside_catch.arrive_and_wait();
                try {
                    std::rethrow_exception(exception);
                } catch (int value) {
                    passed[index] = initially_zero && value == expected &&
                                    thread_local_state.value == expected && std::uncaught_exceptions() == 0;
                }
            }
        } };
    }

    inside_catch.arrive_and_wait();
    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(thread_local_state.value, PROMISE_VALUE);
    EXPECT_EQ(&thread_local_state, main_state);
    EXPECT_EQ(thread_local_destructor_count.load() - destructor_count_before, worker_count);
    for (std::size_t index{}; index < worker_count; ++index) {
        EXPECT_NE(states[index], main_state);
        EXPECT_TRUE(passed[index]);
    }
    EXPECT_NE(states[0], states[1]);
}

TEST(RuntimeLibstdcxx, ClocksAndSleep)
{
    const auto steady_before{ std::chrono::steady_clock::now() };
    const auto system_before{ std::chrono::system_clock::now() };
    std::this_thread::sleep_for(2ms);
    const auto steady_after{ std::chrono::steady_clock::now() };
    const auto system_after{ std::chrono::system_clock::now() };

    EXPECT_GE(steady_after - steady_before, 2ms);
    EXPECT_GT(system_before.time_since_epoch(), 400'000h);
    EXPECT_GT(system_after, system_before);
    static_assert(std::is_same_v<std::chrono::high_resolution_clock, std::chrono::system_clock>);
}
