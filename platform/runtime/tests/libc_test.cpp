#include <gtest/gtest.h>

#include "libc/cpu_clock.h"

#include <array>
#include <barrier>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sys/time.h>
#include <thread>

extern "C" int _gettimeofday(struct timeval* value, void* timezone);

namespace
{
    constexpr std::uint32_t FNV_OFFSET_BASIS{ 2166136261U };
    constexpr std::uint32_t FNV_PRIME{ 16777619U };
    constexpr std::size_t THREAD_COUNT{ 2U };
    constexpr std::size_t ITERATION_COUNT{ 128U };

    [[nodiscard]] auto hash_buffer(const char* buffer, std::size_t size) noexcept -> std::uint32_t
    {
        std::uint32_t hash{ FNV_OFFSET_BASIS };
        for (std::size_t index{}; index < size; ++index) {
            hash ^= static_cast<std::uint8_t>(buffer[index]);
            hash *= FNV_PRIME;
        }
        return hash;
    }
}

TEST(RuntimeLibc, ConcurrentAllocationAndStdio)
{
    std::FILE* const stream{ std::tmpfile() };
    ASSERT_NE(stream, nullptr);

    std::barrier start{ static_cast<std::ptrdiff_t>(THREAD_COUNT + 1U) };
    std::array<bool, THREAD_COUNT> passed{};
    std::array<std::thread, THREAD_COUNT> workers;

    for (std::size_t worker_id{}; worker_id < workers.size(); ++worker_id) {
        workers[worker_id] = std::thread{ [&, worker_id] {
            start.arrive_and_wait();
            bool worker_passed{ true };

            for (std::size_t iteration{}; iteration < ITERATION_COUNT; ++iteration) {
                const std::size_t buffer_size{ 48U + ((worker_id * 7U + iteration) % 32U) };
                auto* const buffer{ static_cast<char*>(std::malloc(buffer_size)) };
                if (buffer == nullptr) {
                    worker_passed = false;
                    break;
                }

                const int written{ std::snprintf(
                  buffer, buffer_size, "worker=%zu iteration=%zu\n", worker_id, iteration) };
                if (written < 0 || static_cast<std::size_t>(written) >= buffer_size) {
                    std::free(buffer);
                    worker_passed = false;
                    break;
                }

                const std::size_t used_size{ static_cast<std::size_t>(written) + 1U };
                const std::uint32_t expected_hash{ hash_buffer(buffer, used_size) };
#if !defined(HAL_PLATFORM_LINUX)
                const int expected_errno{ static_cast<int>(0x40U + worker_id) };
                errno = expected_errno;
#endif

                std::this_thread::yield();

                if (hash_buffer(buffer, used_size) != expected_hash ||
#if !defined(HAL_PLATFORM_LINUX)
                    errno != expected_errno ||
#endif
                    std::fwrite(buffer, 1U, static_cast<std::size_t>(written), stream) !=
                      static_cast<std::size_t>(written)) {
                    worker_passed = false;
                }
                std::free(buffer);

                if (!worker_passed) {
                    break;
                }
            }
            passed[worker_id] = worker_passed;
        } };
    }

    start.arrive_and_wait();
    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_TRUE(passed[0]);
    EXPECT_TRUE(passed[1]);
    EXPECT_EQ(std::fflush(stream), 0);
    EXPECT_GT(std::ftell(stream), 0L);
    EXPECT_EQ(std::fclose(stream), 0);
}

TEST(RuntimeLibc, RetargetedGettimeofdayUsesRealtimeClock)
{
    errno = 0;
    EXPECT_EQ(_gettimeofday(nullptr, nullptr), -1);
    EXPECT_EQ(errno, EFAULT);

    const std::time_t before{ std::time(nullptr) };
    timeval value{};
    ASSERT_EQ(_gettimeofday(&value, nullptr), 0);
    const std::time_t after{ std::time(nullptr) };
    ASSERT_NE(before, static_cast<std::time_t>(-1));
    ASSERT_NE(after, static_cast<std::time_t>(-1));
    EXPECT_GE(value.tv_sec, before - 1);
    EXPECT_LE(value.tv_sec, after + 1);
    EXPECT_GE(value.tv_usec, 0);
    EXPECT_LT(value.tv_usec, 1'000'000);
}

TEST(RuntimeLibc, CpuClockAccumulatesShortRunsAndExcludesIdleAcrossCounterWrap)
{
    runtime_cpu_clock_counter counter{};
    counter.last_cycle = UINT32_MAX - 99U;
    counter.running = true;
    runtime_cpu_clock_update(&counter, 50U);
    EXPECT_EQ(counter.execution_cycles, 150U);
    counter.running = false;
    runtime_cpu_clock_update(&counter, 1050U);
    EXPECT_EQ(counter.execution_cycles, 150U);
    EXPECT_EQ(counter.elapsed_cycles, 1150U);
    counter.running = true;
    for (std::uint32_t cycle{ 1051U }; cycle <= 1100U; ++cycle) {
        runtime_cpu_clock_update(&counter, cycle);
    }
    EXPECT_EQ(counter.execution_cycles, 200U);
    EXPECT_EQ(runtime_cpu_clock_ticks(counter.execution_cycles, 1000U, 100U), 20U);
    EXPECT_EQ(runtime_cpu_clock_ticks(counter.elapsed_cycles, 1000U, 100U), 120U);

    counter.running = false;
    runtime_cpu_clock_update(&counter, UINT32_MAX);
    runtime_cpu_clock_update(&counter, 499U);
    EXPECT_EQ(counter.execution_cycles, 200U);
    EXPECT_GT(counter.elapsed_cycles, UINT32_MAX);
    EXPECT_EQ(runtime_cpu_clock_ticks(480'000'000ULL * 60U + 240'000'000U, 480'000'000U, 100U), 6050U);
}

#if defined(HAL_PLATFORM_LINUX)
TEST(RuntimeLibc, ThreadXSuspendSignalsPreserveErrno)
{
    errno = ERANGE;
    for (std::size_t iteration{}; iteration < 25'000'000U; ++iteration) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    const int preserved_errno{ errno };
    EXPECT_EQ(preserved_errno, ERANGE);
}
#endif
