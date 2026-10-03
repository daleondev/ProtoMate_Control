#include "hal/drivers/factory/timer.hpp"
#include "hardware_driver_checks.hpp"
#include "hardware_runtime_checks.hpp"
#include "hardware_storage_checks.hpp"

#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <barrier>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <ranges>
#include <regex>
#include <semaphore>
#include <shared_mutex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <cerrno>
#include <ctime>
#include <sys/lock.h>
#include <unistd.h>

extern "C" int _getentropy(void* buffer, std::size_t length);
extern "C" int _write(int file, const char* buffer, int length);

extern "C" void runtime_application_define() { runtime::hardware_checks::capture_late_startup_tls(); }

extern "C" {
// These stable values are convenient acceptance points for a debugger or
// an automated probe when the serial connection is unavailable.
std::uint32_t runtime_hardware_self_test_status{};
std::uint32_t runtime_hardware_self_test_phase{};

[[gnu::noinline, gnu::used]] void runtime_hardware_self_test_complete() { asm volatile("" ::: "memory"); }
}

namespace
{
    using namespace std::chrono_literals;

    constexpr std::uint32_t PASS_STATUS{ 0x600D600DU };
    constexpr std::uint32_t FAIL_STATUS{ 0xBAD00000U };
    constexpr std::uint64_t WIDE_ATOMIC_INITIAL{ 0x1'0000'0000ULL };

    struct StartupTlsProbe
    {
        StartupTlsProbe() noexcept
          : self{ this }
        {
        }

        StartupTlsProbe* self;
        std::uint32_t value{ 0x71A5U };
    };

    thread_local StartupTlsProbe startup_tls_probe;
    StartupTlsProbe* startup_tls_address{};

    struct StartupTlsCapture
    {
        StartupTlsCapture() noexcept
        {
            startup_tls_address = &startup_tls_probe;
            startup_tls_probe.value = 0xC0FFEEU;
        }
    } startup_tls_capture;

    std::atomic_uint worker_tls_destructor_count{};

    struct WorkerTlsProbe
    {
        ~WorkerTlsProbe() { worker_tls_destructor_count.fetch_add(1U, std::memory_order_relaxed); }
        std::uint32_t value{};
    };

    thread_local WorkerTlsProbe worker_tls_probe;

    struct ExitOrderState
    {
        std::mutex mutex;
        std::condition_variable condition;
        std::atomic_bool notification_registered{};
        std::atomic_bool destructor_finished{};
        std::atomic_bool observer_waiting{};
        std::atomic_bool notification_observed{};
    };

    struct ExitOrderTlsProbe
    {
        ~ExitOrderTlsProbe()
        {
            if (state != nullptr) {
                std::this_thread::sleep_for(5ms);
                state->destructor_finished.store(true, std::memory_order_release);
            }
        }

        ExitOrderState* state{};
    };

    thread_local ExitOrderTlsProbe exit_order_tls_probe;

    struct AtomicRecord
    {
        std::uint32_t first;
        std::uint32_t second;
        std::uint32_t third;

        constexpr bool operator==(const AtomicRecord&) const = default;
    };

    constexpr AtomicRecord FIRST_RECORD{ .first = 1U, .second = 2U, .third = 3U };
    constexpr AtomicRecord SECOND_RECORD{ .first = 4U, .second = 5U, .third = 6U };

    template<typename... Arguments>
    void log(const char* pattern, Arguments... arguments)
    {
        static_cast<void>(std::printf(pattern, arguments...));
        static_cast<void>(std::putchar('\n'));
        static_cast<void>(std::fflush(stdout));
    }

    [[nodiscard]] bool test_library_surface()
    {
        const std::string formatted{ std::format("{}:{:04x}", "runtime", 42) };
        if (formatted != "runtime:002a") {
            return false;
        }

        std::vector<int> values{ 5, 1, 4, 2, 3 };
        std::ranges::sort(values);
        if (!std::ranges::equal(values, std::array{ 1, 2, 3, 4, 5 }) ||
            std::accumulate(values.begin(), values.end(), 0) != 15) {
            return false;
        }

        const std::expected<int, std::string> result{ 42 };
        const std::optional<std::string> optional{ "optional" };
        const std::variant<int, std::string> variant{ std::in_place_type<std::string>, "variant" };
        const std::any any{ std::uint32_t{ 99U } };
        if (result.value() != 42 || optional.value() != "optional" ||
            std::get<std::string>(variant) != "variant" || std::any_cast<std::uint32_t>(any) != 99U) {
            return false;
        }

        std::array<char, 16U> encoded{};
        const auto encode_result{ std::to_chars(encoded.data(), encoded.data() + encoded.size(), 12345) };
        int decoded{};
        const auto decode_result{ std::from_chars(encoded.data(), encode_result.ptr, decoded) };
        if (encode_result.ec != std::errc{} || decode_result.ec != std::errc{} || decoded != 12345) {
            return false;
        }

        std::istringstream stream{ "17 alpha" };
        int number{};
        std::string word;
        stream >> number >> word;
        return stream && number == 17 && word == "alpha" &&
               std::regex_match("threadx-753", std::regex{ R"(threadx-[0-9]+)" });
    }

    [[nodiscard]] bool test_threads_and_synchronization()
    {
        if (!runtime::hardware_checks::thread_names() ||
            !runtime::hardware_checks::static_initializer_can_join()) {
            return false;
        }
        std::mutex mutex;
        std::condition_variable condition;
        bool ready{};
        std::thread producer{ [&] {
            {
                const std::lock_guard lock{ mutex };
                ready = true;
            }
            condition.notify_one();
        } };
        {
            std::unique_lock lock{ mutex };
            condition.wait(lock, [&] { return ready; });
        }
        producer.join();

        std::timed_mutex timed_mutex;
        timed_mutex.lock();
        bool timed_out{};
        std::thread timed_waiter{ [&] { timed_out = !timed_mutex.try_lock_for(2ms); } };
        timed_waiter.join();
        timed_mutex.unlock();
        if (!timed_out) {
            return false;
        }

        std::recursive_mutex recursive_mutex;
        recursive_mutex.lock();
        recursive_mutex.lock();
        recursive_mutex.unlock();
        recursive_mutex.unlock();

        std::shared_mutex shared_mutex;
        shared_mutex.lock_shared();
        shared_mutex.unlock_shared();
        shared_mutex.lock();
        shared_mutex.unlock();

        std::shared_timed_mutex shared_timed_mutex;
        shared_timed_mutex.lock();
        bool shared_timed_out{};
        std::thread shared_waiter{ [&] { shared_timed_out = !shared_timed_mutex.try_lock_shared_for(2ms); } };
        shared_waiter.join();
        shared_timed_mutex.unlock();
        if (!shared_timed_out) {
            return false;
        }

        std::binary_semaphore semaphore{ 0 };
        std::thread releaser{ [&] { semaphore.release(); } };
        semaphore.acquire();
        releaser.join();

        std::counting_semaphore<4> counting_semaphore{ 0 };
        counting_semaphore.release(2);
        counting_semaphore.acquire();
        counting_semaphore.acquire();
        if (counting_semaphore.try_acquire() || counting_semaphore.try_acquire_for(2ms)) {
            return false;
        }

        std::latch completion{ 2 };
        std::jthread latch_first{ [&] { completion.count_down(); } };
        std::jthread latch_second{ [&] { completion.count_down(); } };
        completion.wait();
        latch_first.join();
        latch_second.join();

        std::atomic_uint arrivals{};
        std::barrier barrier{ 3 };
        std::jthread first{ [&] {
            arrivals.fetch_add(1U, std::memory_order_relaxed);
            barrier.arrive_and_wait();
        } };
        std::jthread second{ [&] {
            arrivals.fetch_add(1U, std::memory_order_relaxed);
            barrier.arrive_and_wait();
        } };
        barrier.arrive_and_wait();
        first.join();
        second.join();

        std::once_flag outer;
        std::once_flag inner;
        unsigned int once_count{};
        std::call_once(outer, [&] {
            ++once_count;
            std::call_once(inner, [&] { ++once_count; });
        });
        std::call_once(outer, [&] { ++once_count; });

        std::once_flag retry_once;
        unsigned int retry_count{};
        try {
            std::call_once(retry_once, [&] {
                ++retry_count;
                throw std::runtime_error{ "retry call_once" };
            });
            return false;
        } catch (const std::runtime_error&) {
        }
        std::call_once(retry_once, [&] { ++retry_count; });

        std::once_flag contended_once;
        std::atomic_uint contended_count{};
        std::array<std::thread, 2U> once_workers;
        for (auto& worker : once_workers) {
            worker = std::thread{ [&] {
                std::call_once(contended_once,
                               [&] { contended_count.fetch_add(1U, std::memory_order_relaxed); });
            } };
        }
        for (auto& worker : once_workers) {
            worker.join();
        }

        std::atomic_bool stopped{};
        std::jthread stoppable{ [&](std::stop_token token) {
            while (!token.stop_requested()) {
                std::this_thread::yield();
            }
            stopped.store(true, std::memory_order_release);
        } };
        const bool stop_requested{ stoppable.request_stop() };
        stoppable.join();

        std::condition_variable_any interruptible_condition;
        std::mutex interruptible_mutex;
        std::binary_semaphore stop_wait_started{ 0 };
        std::atomic_bool wait_was_stopped{};
        std::jthread condition_waiter{ [&](std::stop_token token) {
            std::unique_lock lock{ interruptible_mutex };
            stop_wait_started.release();
            wait_was_stopped.store(
              !interruptible_condition.wait(lock, std::move(token), [] { return false; }),
              std::memory_order_release);
        } };
        stop_wait_started.acquire();
        std::this_thread::sleep_for(2ms);
        const bool condition_stop_requested{ condition_waiter.request_stop() };
        condition_waiter.join();

        return arrivals.load(std::memory_order_relaxed) == 2U && once_count == 2U && retry_count == 2U &&
               contended_count.load(std::memory_order_relaxed) == 1U && stop_requested &&
               stopped.load(std::memory_order_acquire) && condition_stop_requested &&
               wait_was_stopped.load(std::memory_order_acquire) && std::thread::hardware_concurrency() == 1U;
    }

    [[nodiscard]] bool test_atomics_futures_and_exceptions()
    {
        if (!runtime::hardware_checks::non_native_atomic_width()) {
            return false;
        }
        std::atomic<int> value{};
        std::binary_semaphore atomic_wait_started{ 0 };
        std::atomic_bool atomic_wait_returned{};
        std::thread waiter{ [&] {
            atomic_wait_started.release();
            value.wait(0);
            atomic_wait_returned.store(true, std::memory_order_release);
            value.fetch_add(1, std::memory_order_acq_rel);
        } };
        atomic_wait_started.acquire();
        std::this_thread::sleep_for(2ms);
        if (atomic_wait_returned.load(std::memory_order_acquire)) {
            waiter.join();
            return false;
        }
        value.store(1, std::memory_order_release);
        value.notify_one();
        waiter.join();
        if (value.load(std::memory_order_acquire) != 2) {
            return false;
        }

        std::atomic<std::uint64_t> wide{ WIDE_ATOMIC_INITIAL };
        if (wide.fetch_add(1U) != WIDE_ATOMIC_INITIAL || wide.exchange(7U) != WIDE_ATOMIC_INITIAL + 1U) {
            return false;
        }
        std::uint64_t expected{ 7U };
        if (!wide.compare_exchange_strong(expected, 9U) || wide.load() != 9U) {
            return false;
        }

        std::atomic_flag flag = ATOMIC_FLAG_INIT;
        static_cast<void>(flag.test_and_set(std::memory_order_release));
        std::binary_semaphore flag_wait_started{ 0 };
        std::atomic_bool flag_wait_returned{};
        std::thread flag_waiter{ [&] {
            flag_wait_started.release();
            flag.wait(true, std::memory_order_acquire);
            flag_wait_returned.store(true, std::memory_order_release);
        } };
        flag_wait_started.acquire();
        std::this_thread::sleep_for(2ms);
        if (flag_wait_returned.load(std::memory_order_acquire)) {
            flag_waiter.join();
            return false;
        }
        flag.clear(std::memory_order_release);
        flag.notify_one();
        flag_waiter.join();

        std::atomic<AtomicRecord> record{ FIRST_RECORD };
        if (record.exchange(SECOND_RECORD) != FIRST_RECORD || record.load() != SECOND_RECORD) {
            return false;
        }

        std::promise<int> promise;
        std::future<int> future{ promise.get_future() };
        std::thread promise_thread{ [promise = std::move(promise)]() mutable {
            try {
                throw 41;
            } catch (...) {
                promise.set_value(std::any_cast<int>(std::any{ 42 }));
            }
        } };
        const bool future_ready{ future.wait_for(100ms) == std::future_status::ready };
        const int future_value{ future_ready ? future.get() : 0 };
        promise_thread.join();

        std::promise<int> exit_promise;
        std::future<int> exit_future{ exit_promise.get_future() };
        std::thread exit_promise_thread{ [promise_at_exit = std::move(exit_promise)]() mutable {
            promise_at_exit.set_value_at_thread_exit(73);
        } };
        exit_promise_thread.join();
        const bool exit_future_ready{ exit_future.wait_for(100ms) == std::future_status::ready };
        const int exit_future_value{ exit_future_ready ? exit_future.get() : 0 };

        auto asynchronous{ std::async(std::launch::async, [] { return std::make_tuple(1, 2, 3); }) };
        return future_ready && future_value == 42 && exit_future_ready && exit_future_value == 73 &&
               asynchronous.get() == std::make_tuple(1, 2, 3);
    }

    [[nodiscard]] bool test_tls_and_thread_exit_order()
    {
        if (!runtime::hardware_checks::tls_destructor_can_join() ||
            !runtime::hardware_checks::late_startup_tls_is_preserved() ||
            !runtime::hardware_checks::native_thread_reset_reinitializes_runtime()) {
            return false;
        }
        if (startup_tls_address == nullptr || startup_tls_address != &startup_tls_probe ||
            startup_tls_probe.self != &startup_tls_probe || startup_tls_probe.value != 0xC0FFEEU) {
            return false;
        }

        const unsigned int destructors_before{ worker_tls_destructor_count.load(std::memory_order_relaxed) };
        std::array<WorkerTlsProbe*, 2U> addresses{};
        std::array<std::thread, 2U> workers;
        for (std::size_t index{}; index < workers.size(); ++index) {
            workers[index] = std::thread{ [&, index] {
                worker_tls_probe.value = static_cast<std::uint32_t>(index + 1U);
                addresses[index] = &worker_tls_probe;
                try {
                    throw static_cast<int>(worker_tls_probe.value);
                } catch (int caught) {
                    worker_tls_probe.value = static_cast<std::uint32_t>(caught);
                }
            } };
        }
        for (auto& worker : workers) {
            worker.join();
        }
        if (addresses[0] == nullptr || addresses[1] == nullptr || addresses[0] == addresses[1] ||
            addresses[0] == &worker_tls_probe || addresses[1] == &worker_tls_probe ||
            worker_tls_destructor_count.load(std::memory_order_relaxed) - destructors_before !=
              workers.size()) {
            return false;
        }

        ExitOrderState state;
        std::thread observer{ [&] {
            std::unique_lock lock{ state.mutex };
            state.observer_waiting.store(true, std::memory_order_release);
            state.condition.wait(lock,
                                 [&] { return state.destructor_finished.load(std::memory_order_acquire); });
            state.notification_observed.store(true, std::memory_order_release);
        } };
        while (!state.observer_waiting.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
        // observer_waiting is published immediately before wait() releases the
        // mutex. Give the lower-priority observer a scheduling window to enter
        // the actual condition wait before the exiting thread is created.
        std::this_thread::sleep_for(2ms);

        std::thread exiting_thread{ [&] {
            std::unique_lock lock{ state.mutex };
            exit_order_tls_probe.state = &state;
            std::notify_all_at_thread_exit(state.condition, std::move(lock));
            state.notification_registered.store(true, std::memory_order_release);
        } };
        while (!state.notification_registered.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
        bool destructor_preceded_unlock{};
        {
            const std::lock_guard lock{ state.mutex };
            destructor_preceded_unlock = state.destructor_finished.load(std::memory_order_acquire);
        }
        exiting_thread.join();
        observer.join();
        return destructor_preceded_unlock && state.destructor_finished.load(std::memory_order_acquire) &&
               state.notification_observed.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool test_libc_reentrancy_and_entropy()
    {
        std::array<bool, 3U> errno_isolated{};
        std::barrier checkpoint{ static_cast<std::ptrdiff_t>(errno_isolated.size() + 1U) };
        std::array<std::thread, errno_isolated.size()> workers;
        for (std::size_t index{}; index < workers.size(); ++index) {
            workers[index] = std::thread{ [&, index] {
                const int expected_errno{ static_cast<int>(EAGAIN + index) };
                errno = expected_errno;
                checkpoint.arrive_and_wait();
                errno_isolated[index] = errno == expected_errno;
            } };
        }
        checkpoint.arrive_and_wait();
        for (auto& worker : workers) {
            worker.join();
        }
        if (!std::ranges::all_of(errno_isolated, std::identity{})) {
            return false;
        }

        std::array<char, 32U> formatted{};
        if (std::snprintf(formatted.data(), formatted.size(), "%s:%d", "newlib", 42) <= 0 ||
            std::string_view{ formatted.data() } != "newlib:42") {
            return false;
        }

        errno = 0;
        if (_write(STDOUT_FILENO, nullptr, 0) != 0 || _write(STDOUT_FILENO, nullptr, 1) != -1 ||
            errno != EFAULT) {
            return false;
        }

        std::array<std::byte, 256U> entropy{};
        if (_getentropy(nullptr, 0U) != 0 || _getentropy(entropy.data(), 16U) != 0) {
            return false;
        }
        if (std::ranges::all_of(std::span{ entropy }.first<16U>(),
                                [&](std::byte value) { return value == entropy.front(); })) {
            return false;
        }
        errno = 0;
        if (_getentropy(nullptr, 1U) != -1 || errno != EFAULT) {
            return false;
        }
        errno = 0;
        if (_getentropy(entropy.data(), entropy.size() + 1U) != -1 || errno != EIO) {
            return false;
        }

        std::array<_LOCK_T, 32U> newlib_locks{};
        for (auto& lock : newlib_locks) {
            __retarget_lock_init(&lock);
            if (lock == nullptr) {
                return false;
            }
            __retarget_lock_acquire(lock);
            __retarget_lock_release(lock);
        }
        for (auto lock : newlib_locks) {
            __retarget_lock_close(lock);
        }
        // Allocate another wave to verify that closed lock nodes are safely
        // recycled rather than exhausting a fixed-size pool.
        for (auto& lock : newlib_locks) {
            lock = nullptr;
            __retarget_lock_init_recursive(&lock);
            if (lock == nullptr) {
                return false;
            }
            __retarget_lock_acquire_recursive(lock);
            __retarget_lock_release_recursive(lock);
        }
        for (auto lock : newlib_locks) {
            __retarget_lock_close_recursive(lock);
        }

        std::random_device random;
        std::array<std::random_device::result_type, 4U> random_values{};
        std::ranges::generate(random_values, [&] { return random(); });
        return random.entropy() == std::numeric_limits<std::random_device::result_type>::digits &&
               !std::ranges::all_of(random_values,
                                    [&](auto value) { return value == random_values.front(); });
    }

    [[nodiscard]] bool test_console_writers()
    {
        // These raw writes bypass stdio's per-FILE lock. A higher-priority
        // writer preempts a long transmission, exercising the UART's own lock.
        // A UART capture must contain exactly 2048 A bytes and 128 B bytes
        // between the markers; the old BSP silently discarded HAL_BUSY bytes.
        log("[uart-stress-begin]");
        bool slow_passed{};
        bool fast_passed{ true };
        std::binary_semaphore started{ 0 };
        auto slow = runtime::thread::create({ .priority = 18 }, [&] {
            std::array<char, 2048U> bytes;
            bytes.fill('A');
            started.release();
            slow_passed = _write(STDOUT_FILENO, bytes.data(), static_cast<int>(bytes.size())) ==
                          static_cast<int>(bytes.size());
        });
        auto fast = runtime::thread::create({ .priority = 17 }, [&] {
            started.acquire();
            for (unsigned int index{}; index < 128U; ++index) {
                std::this_thread::sleep_for(1ms);
                fast_passed = (_write(STDERR_FILENO, "B", 1) == 1) && fast_passed;
            }
        });
        slow.join();
        fast.join();
        log("\n[uart-stress-end]");
        return slow_passed && fast_passed;
    }

    [[nodiscard]] bool test_many_streams_and_threads()
    {
        namespace fs = std::filesystem;
        const fs::path directory{ "/flash/stream-stress" };
        std::error_code error;
        static_cast<void>(fs::remove_all(directory, error));
        error.clear();
        if (!fs::create_directory(directory, error) || error) {
            log("[runtime-self-test] stream stress create failed: %d", error.value());
            return false;
        }

        std::array<std::fstream, 16U> streams;
        for (std::size_t index{}; index < streams.size(); ++index) {
            streams[index].open(directory / std::format("{}.txt", index),
                                std::ios::in | std::ios::out | std::ios::trunc);
            if (!streams[index]) {
                log("[runtime-self-test] stream stress open failed: %u", static_cast<unsigned int>(index));
                return false;
            }
        }
        for (std::size_t index{}; index < streams.size(); ++index) {
            auto& stream{ streams[index] };
            stream.close();
            if (stream.fail()) {
                log("[runtime-self-test] stream stress close failed: %u", static_cast<unsigned int>(index));
                return false;
            }
        }
        for (std::size_t index{}; index < streams.size(); ++index) {
            if (!fs::remove(directory / std::format("{}.txt", index), error) || error) {
                log("[runtime-self-test] stream stress file remove failed: %u/%d",
                    static_cast<unsigned int>(index),
                    error.value());
                return false;
            }
        }
        if (!fs::remove(directory, error) || error) {
            log("[runtime-self-test] stream stress directory remove failed: %d", error.value());
            return false;
        }

        for (std::size_t iteration{}; iteration < 128U; ++iteration) {
            std::thread thread{ [iteration] {
                const auto allocation{ std::make_unique<std::uint32_t>(
                  static_cast<std::uint32_t>(iteration)) };
                if (*allocation != iteration) {
                    std::terminate();
                }
            } };
            thread.join();
        }
        return test_console_writers();
    }

    [[nodiscard]] bool test_cpu_clock()
    {
        const auto milliseconds_between = [](std::clock_t before, std::clock_t after) {
            return before == static_cast<std::clock_t>(-1) || after == static_cast<std::clock_t>(-1) ||
                       after < before
                     ? -1.0
                     : 1000.0 * static_cast<double>(after - before) / CLOCKS_PER_SEC;
        };
        const auto idle_before{ std::clock() };
        std::this_thread::sleep_for(200ms);
        const double idle_cpu{ milliseconds_between(idle_before, std::clock()) };

        const auto spin = [](std::chrono::milliseconds duration) {
            const auto deadline{ std::chrono::steady_clock::now() + duration };
            while (std::chrono::steady_clock::now() < deadline) {
                std::atomic_signal_fence(std::memory_order_seq_cst);
            }
        };
        const auto worker_before{ std::clock() };
        std::thread worker{ [&] { spin(200ms); } };
        worker.join();
        const double worker_cpu{ milliseconds_between(worker_before, std::clock()) };

        // Longer than one full DWT revolution at the board's 480 MHz. No clock()
        // reads in the loop: SysTick must extend the counter independently.
        const auto busy_before{ std::clock() };
        spin(9500ms);
        const double busy_cpu{ milliseconds_between(busy_before, std::clock()) };
        log("[runtime-self-test] CPU ms: idle=%.1f worker=%.1f counter-wrap=%.1f",
            idle_cpu,
            worker_cpu,
            busy_cpu);
        return idle_cpu >= 0.0 && idle_cpu < 50.0 && worker_cpu >= 170.0 && worker_cpu <= 250.0 &&
               busy_cpu >= 9400.0 && busy_cpu <= 9600.0;
    }

    [[nodiscard]] bool test_clocks()
    {
        const auto steady_before{ std::chrono::steady_clock::now() };
        const auto system_before{ std::chrono::system_clock::now() };
        const std::time_t c_time{ std::time(nullptr) };
        std::this_thread::sleep_for(5ms);
        const auto steady_after{ std::chrono::steady_clock::now() };
        const auto system_after{ std::chrono::system_clock::now() };
        const std::time_t chrono_time{ std::chrono::system_clock::to_time_t(system_after) };
        const auto difference{ c_time > chrono_time ? c_time - chrono_time : chrono_time - c_time };
        return std::chrono::steady_clock::is_steady && steady_after - steady_before >= 5ms &&
               system_after > system_before && c_time >= 946'684'800 && difference <= 2 && test_cpu_clock();
    }

    [[nodiscard]] bool test_hardware_timer()
    {
        if (!hardware_driver_checks::gpio_output_toggle() ||
            !hardware_driver_checks::gpio_interrupt_ownership() ||
            !hardware_driver_checks::ethernet_loopback()) {
            return false;
        }
        constexpr std::size_t TIMER_INDEX{ 5U };
        constexpr std::uint32_t TEST_TICK_FREQUENCY_HZ{ 10'000U };

        const auto timer{ hal::timer::create(TIMER_INDEX) };
        if (timer == nullptr || timer->getInputFrequencyHz() < TEST_TICK_FREQUENCY_HZ) {
            return false;
        }

        const auto original_prescaler{ timer->getPrescaler() };
        const auto original_auto_reload{ timer->getAutoReload() };
        const auto original_counter{ timer->getCounter() };
        const bool originally_running{ timer->isRunning() };
        if (!timer->stop()) {
            return false;
        }

        timer->setPrescaler(std::numeric_limits<hal::ITimer::Tick>::max());
        const bool prescaler_saturated{ timer->getPrescaler() == std::numeric_limits<std::uint16_t>::max() };
        const auto divisor{ timer->getInputFrequencyHz() / TEST_TICK_FREQUENCY_HZ };
        timer->setPrescaler(divisor - 1U);
        timer->setAutoReload(std::numeric_limits<hal::ITimer::Tick>::max());
        timer->setCounter(0U);

        bool passed{ prescaler_saturated && timer->getTickFrequencyHz() == TEST_TICK_FREQUENCY_HZ &&
                     timer->start() && timer->start() };
        std::this_thread::sleep_for(20ms);
        const auto measured_ticks{ timer->getCounter() };
        // At 10 kHz this should be hundreds of ticks. If PSC remained buffered,
        // the CubeMX 1 MHz setting would advance by tens of thousands instead.
        passed = passed && measured_ticks > 0U && measured_ticks < TEST_TICK_FREQUENCY_HZ;

        std::atomic_uint callbacks{};
        timer->setAutoReload(49U);
        timer->forceUpdateEvent();
        timer->setPeriodElapsedCallback(
          [&callbacks]() noexcept { callbacks.fetch_add(1U, std::memory_order_relaxed); });
        passed = passed && timer->startIt() && timer->startIt();

        const auto callback_deadline{ std::chrono::steady_clock::now() + 100ms };
        while (callbacks.load(std::memory_order_relaxed) == 0U &&
               std::chrono::steady_clock::now() < callback_deadline) {
            std::this_thread::sleep_for(1ms);
        }
        passed = passed && callbacks.load(std::memory_order_relaxed) != 0U;

        passed = passed && timer->start();
        const auto callbacks_before_polling{ callbacks.load(std::memory_order_relaxed) };
        std::this_thread::sleep_for(20ms);
        passed = passed && callbacks.load(std::memory_order_relaxed) == callbacks_before_polling;

        timer->clearPeriodElapsedCallback();
        passed = passed && timer->stop() && timer->stop();
        timer->setPrescaler(original_prescaler);
        timer->setAutoReload(original_auto_reload);
        timer->setCounter(original_counter);
        if (originally_running) {
            passed = passed && timer->start();
        }
        return passed;
    }

    [[nodiscard]] bool test_filex_standard_library()
    {
        if (!runtime::tests::check_storage_offsets("/flash/runtime-offset-test.bin")) {
            return false;
        }
        namespace fs = std::filesystem;
        const fs::path root{ "/flash/runtime-self-test" };
        const fs::path nested{ root / "nested" };
        const fs::path source{ nested / "source.txt" };
        const fs::path copy{ nested / "copy.txt" };
        const fs::path target{ nested / "target.txt" };
        constexpr std::string_view CONTENT{ "FileX std::filesystem conformance" };
        std::error_code error;

        static_cast<void>(fs::remove_all(root, error));
        error.clear();
        if (!fs::create_directories(nested, error) || error) {
            return false;
        }
        {
            std::fstream stream{ source, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc };
            stream.write(CONTENT.data(), static_cast<std::streamsize>(CONTENT.size()));
            stream.seekg(0);
            std::string input(CONTENT.size(), '\0');
            stream.read(input.data(), static_cast<std::streamsize>(input.size()));
            if (!stream || input != CONTENT) {
                return false;
            }
        }
        if (!fs::copy_file(source, copy, error) || error) {
            return false;
        }
        {
            std::ofstream replacement{ target };
            replacement << "old";
        }
        fs::rename(copy, target, error);
        if (error || fs::file_size(target, error) != CONTENT.size() || error) {
            return false;
        }
        {
            std::ifstream renamed{ target, std::ios::binary };
            std::string input(CONTENT.size(), '\0');
            renamed.read(input.data(), static_cast<std::streamsize>(input.size()));
            if (!renamed || input != CONTENT) {
                return false;
            }
        }

        std::size_t entries{};
        for ([[maybe_unused]] const auto& entry : fs::recursive_directory_iterator{ root, error }) {
            ++entries;
        }
        if (error || entries != 3U) {
            return false;
        }

        const fs::path mixed_case{ nested / "Case-Identity.txt" };
        const fs::path upper_case{ nested / "CASE-IDENTITY.TXT" };
        {
            std::ofstream stream{ mixed_case };
            stream << "case";
        }
        fs::rename(nested / "case-identity.txt", upper_case, error);
        if (error || !fs::equivalent(upper_case, nested / "case-identity.txt", error) || error ||
            !fs::remove(upper_case, error) || error) {
            return false;
        }

        const fs::space_info volume{ fs::space(root, error) };
        if (error || volume.capacity != 24U * 1024U * 512U || volume.available > volume.capacity) {
            return false;
        }

        fs::current_path(nested, error);
        if (error) {
            return false;
        }
        {
            std::ifstream relative{ "source.txt", std::ios::binary };
            std::string input(CONTENT.size(), '\0');
            relative.read(input.data(), static_cast<std::streamsize>(input.size()));
            if (!relative || input != CONTENT) {
                fs::current_path("/", error);
                return false;
            }
        }
        fs::current_path("/", error);
        if (error) {
            return false;
        }

        const std::uintmax_t removed{ fs::remove_all(root, error) };
        if (error || removed != 4U || fs::exists(root, error) || error) {
            return false;
        }

        const fs::path tree{ "/flash/self-move" };
        const fs::path child{ tree / "child" };
        if (!fs::create_directories(child, error) || error) {
            return false;
        }
        fs::rename(tree, child / "grandchild", error);
        const bool rejected_self_move{ error == std::errc::invalid_argument };
        error.clear();
        const bool tree_survived{ fs::is_directory(child, error) && !error };
        static_cast<void>(fs::remove_all(tree, error));
        return rejected_self_move && tree_survived && !error;
    }

    [[nodiscard]] bool test_sd_standard_library()
    {
        if (!runtime::tests::check_storage_offsets("/sd/runtime-offset-test.bin")) {
            return false;
        }
        namespace fs = std::filesystem;
        const fs::path path{ "/sd/runtime-self-test.bin" };
        constexpr std::array<std::uint8_t, 8U> CONTENT{
            0x53U, 0x44U, 0x4DU, 0x4DU, 0x43U, 0x2DU, 0x4FU, 0x4BU
        };
        std::error_code error;
        static_cast<void>(fs::remove(path, error));
        error.clear();
        {
            std::ofstream stream{ path, std::ios::binary | std::ios::trunc };
            stream.write(reinterpret_cast<const char*>(CONTENT.data()),
                         static_cast<std::streamsize>(CONTENT.size()));
            if (!stream) {
                return false;
            }
        }
        std::array<std::uint8_t, CONTENT.size()> input{};
        {
            std::ifstream stream{ path, std::ios::binary };
            stream.read(reinterpret_cast<char*>(input.data()), static_cast<std::streamsize>(input.size()));
            if (!stream || input != CONTENT) {
                return false;
            }
        }
        const fs::space_info volume{ fs::space("/sd", error) };
        const bool passed{ !error && volume.capacity >= 8U * 1024U * 1024U * 1024U &&
                           volume.available <= volume.capacity };
        error.clear();
        static_cast<void>(fs::remove(path, error));
        return passed && !error;
    }

    struct NamedTest
    {
        const char* name;
        bool (*function)();
    };

    constexpr std::array TESTS{
        NamedTest{ "library surface", test_library_surface },
        NamedTest{ "threads and synchronization", test_threads_and_synchronization },
        NamedTest{ "atomics, futures, and exceptions", test_atomics_futures_and_exceptions },
        NamedTest{ "TLS and thread-exit order", test_tls_and_thread_exit_order },
        NamedTest{ "libc reentrancy and entropy", test_libc_reentrancy_and_entropy },
        NamedTest{ "stream and thread stress", test_many_streams_and_threads },
        NamedTest{ "clocks", test_clocks },
        NamedTest{ "HAL peripherals", test_hardware_timer },
        NamedTest{ "LevelX/FileX flash standard library", test_filex_standard_library },
        NamedTest{ "FileX SD standard library", test_sd_standard_library },
    };

    [[noreturn]] void finish(bool passed, std::size_t phase)
    {
        runtime_hardware_self_test_status =
          passed ? PASS_STATUS : FAIL_STATUS | static_cast<std::uint32_t>(phase);
        runtime_hardware_self_test_complete();
        log(passed ? "[runtime-self-test] PASS" : "[runtime-self-test] FAIL");
        while (true) {
            std::this_thread::sleep_for(1s);
        }
    }
}

int main()
{
    log("[runtime-self-test] ARM/Newlib/ThreadX conformance start");
    for (std::size_t index{}; index < TESTS.size(); ++index) {
        runtime_hardware_self_test_phase = static_cast<std::uint32_t>(index + 1U);
        log("[runtime-self-test] RUN  %s", TESTS[index].name);
        bool passed{};
        try {
            passed = TESTS[index].function();
        } catch (const std::exception& exception) {
            log("[runtime-self-test] exception: %s", exception.what());
        } catch (...) {
            log("[runtime-self-test] unknown exception");
        }
        if (!passed) {
            log("[runtime-self-test] FAIL %s", TESTS[index].name);
            finish(false, index + 1U);
        }
        log("[runtime-self-test] PASS %s", TESTS[index].name);
    }
    finish(true, TESTS.size());
}
