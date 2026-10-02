#include "hal/drivers/factory/rtc.hpp"
#include "hal/drivers/factory/timer.hpp"
#include "hal/drivers/impl/linux/Timer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <thread>
#include <utility>

using namespace std::chrono_literals;

namespace
{
    [[nodiscard]] auto total_nanoseconds(const hal::IRtc::Timestamp& timestamp) -> std::int64_t
    {
        return (timestamp.seconds_since_epoch * hal::IRtc::NANOSECONDS_PER_SECOND) + timestamp.nanoseconds;
    }

    [[nodiscard]] auto monotonic_nanoseconds() -> std::int64_t
    {
        timespec now{};
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return -1;
        }
        return static_cast<std::int64_t>(now.tv_sec) * hal::IRtc::NANOSECONDS_PER_SECOND + now.tv_nsec;
    }

    [[nodiscard]] auto realtime_nanoseconds() -> std::int64_t
    {
        timespec now{};
        if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
            return -1;
        }
        return static_cast<std::int64_t>(now.tv_sec) * hal::IRtc::NANOSECONDS_PER_SECOND + now.tv_nsec;
    }
}

TEST(HalClockDrivers, RtcSupportsConcurrentReaders)
{
    const auto rtc{ hal::rtc::create() };
    ASSERT_NE(rtc, nullptr);

    constexpr std::size_t reader_count{ 4U };
    constexpr std::size_t sample_count{ 256U };
    std::atomic_bool start{};
    std::array<bool, reader_count> passed{};
    std::array<std::thread, reader_count> readers;

    for (std::size_t index{}; index < readers.size(); ++index) {
        readers[index] = std::thread{ [&, index] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            std::int64_t previous{};
            bool valid{ true };
            for (std::size_t sample{}; sample < sample_count; ++sample) {
                const auto timestamp{ rtc->getTime() };
                if (!timestamp || timestamp->nanoseconds >= hal::IRtc::NANOSECONDS_PER_SECOND) {
                    valid = false;
                    break;
                }
                const auto current{ total_nanoseconds(*timestamp) };
                if (sample != 0U && current < previous) {
                    valid = false;
                    break;
                }
                previous = current;
            }
            passed[index] = valid;
        } };
    }

    start.store(true, std::memory_order_release);
    for (auto& reader : readers) {
        reader.join();
    }
    for (const bool valid : passed) {
        EXPECT_TRUE(valid);
    }
}

TEST(HalClockDrivers, RtcProvidesRealtime)
{
    const auto rtc{ hal::rtc::create() };
    ASSERT_NE(rtc, nullptr);

    const auto realtime_before{ realtime_nanoseconds() };
    const auto before{ rtc->getTime() };
    const auto realtime_after{ realtime_nanoseconds() };
    ASSERT_TRUE(before.has_value());
    ASSERT_GE(realtime_before, 0);
    ASSERT_GE(realtime_after, 0);
    EXPECT_LT(before->nanoseconds, hal::IRtc::NANOSECONDS_PER_SECOND);
    EXPECT_GE(total_nanoseconds(*before), realtime_before);
    EXPECT_LE(total_nanoseconds(*before), realtime_after);

    std::this_thread::sleep_for(2ms);
    const auto after{ rtc->getTime() };
    ASSERT_TRUE(after.has_value());

    EXPECT_GT(total_nanoseconds(*after), total_nanoseconds(*before));
}

TEST(HalClockDrivers, TimerProvidesHighResolutionCounter)
{
    constexpr std::size_t high_resolution_timer_index{ 2U };
    const auto timer{ hal::timer::create(high_resolution_timer_index) };
    ASSERT_NE(timer, nullptr);
    ASSERT_GT(timer->getTickFrequencyHz(), 0U);

    const std::uint64_t modulus{ static_cast<std::uint64_t>(timer->getAutoReload()) + 1U };
    const auto before{ timer->getCounter() };
    std::this_thread::sleep_for(2ms);
    const auto after{ timer->getCounter() };
    const std::uint64_t elapsed{ after >= before ? after - before : modulus - before + after };
    EXPECT_GT(elapsed, 0U);
}

TEST(HalClockDrivers, TimerConversionsAndFactoryValidation)
{
    EXPECT_EQ(hal::timer::create(0U), nullptr);

    hal::Timer invalid_timer{ hal::Timer::Configuration{
      .input_frequency_hz = 0U, .prescaler = 0U, .auto_reload = 1U } };
    EXPECT_FALSE(invalid_timer.start().has_value());
    EXPECT_FALSE(invalid_timer.startIt().has_value());
    EXPECT_FALSE(invalid_timer.isRunning());

    hal::Timer timer{ hal::Timer::Configuration{ .input_frequency_hz = 1'000'000U,
                                                 .prescaler = 0U,
                                                 .auto_reload =
                                                   std::numeric_limits<hal::ITimer::Tick>::max() } };

    EXPECT_EQ(timer.durationToTicks(0ns), 0U);
    EXPECT_EQ(timer.durationToTicks(1ns), 1U);
    EXPECT_EQ(timer.durationToTicks(1us), 1U);
    EXPECT_EQ(timer.durationToTicks(1'500ns), 2U);

    timer.setPeriod(2'500ns);
    EXPECT_EQ(timer.getAutoReload(), 2U);
    EXPECT_GT(timer.getStateVersion(), 0U);
}

TEST(HalClockDrivers, LinuxTimerInterruptUsesRemainingCounterPeriod)
{
    constexpr hal::ITimer::Tick period_ticks{ 200U };
    constexpr hal::ITimer::Tick initial_counter{ 150U };
    hal::Timer timer{ hal::Timer::Configuration{
      .input_frequency_hz = 1'000U, .prescaler = 0U, .auto_reload = period_ticks - 1U } };
    timer.setCounter(initial_counter);

    std::atomic<std::int64_t> callback_time{ -1 };
    timer.setPeriodElapsedCallback([&]() noexcept {
        std::int64_t expected{ -1 };
        static_cast<void>(callback_time.compare_exchange_strong(expected, monotonic_nanoseconds()));
    });

    const std::int64_t started_at{ monotonic_nanoseconds() };
    ASSERT_TRUE(timer.startIt().has_value());
    const std::int64_t timeout{ started_at + 500ms / 1ns };
    while (callback_time.load(std::memory_order_acquire) < 0 && monotonic_nanoseconds() < timeout) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(timer.stopIt().has_value());

    const std::int64_t fired_at{ callback_time.load(std::memory_order_acquire) };
    ASSERT_GE(fired_at, 0);
    EXPECT_LT(fired_at - started_at, 150ms / 1ns);
}

TEST(HalClockDrivers, TimerAcceptsTypeErasedPeriodElapsedCallback)
{
    hal::Timer timer{ hal::Timer::Configuration{
      .input_frequency_hz = 1'000U, .prescaler = 0U, .auto_reload = 99U } };
    hal::ITimer::PeriodElapsedCallback callback{ []() noexcept {} };

    timer.setPeriodElapsedCallback(std::move(callback));
    timer.clearPeriodElapsedCallback();
}

TEST(HalClockDrivers, ChronoFallsBackAfterTimerStateChange)
{
    constexpr std::size_t high_resolution_timer_index{ 2U };
    const auto timer{ hal::timer::create(high_resolution_timer_index) };
    ASSERT_NE(timer, nullptr);

    const auto before{ std::chrono::system_clock::now() };

    constexpr std::size_t reader_count{ 4U };
    constexpr std::size_t sample_count{ 128U };
    std::atomic_bool start{};
    std::atomic_bool stop_succeeded{};
    std::array<bool, reader_count> passed{};
    std::array<std::thread, reader_count> readers;
    for (std::size_t index{}; index < readers.size(); ++index) {
        readers[index] = std::thread{ [&, index] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            auto previous{ std::chrono::system_clock::now() };
            bool valid{ true };
            for (std::size_t sample{}; sample < sample_count; ++sample) {
                const auto current{ std::chrono::system_clock::now() };
                valid = valid && current >= previous;
                previous = current;
                std::this_thread::yield();
            }
            passed[index] = valid;
        } };
    }
    std::thread timer_mutator{ [&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        stop_succeeded.store(timer->stop().has_value(), std::memory_order_release);
    } };

    start.store(true, std::memory_order_release);
    for (auto& reader : readers) {
        reader.join();
    }
    timer_mutator.join();

    EXPECT_TRUE(stop_succeeded.load(std::memory_order_acquire));
    for (const bool valid : passed) {
        EXPECT_TRUE(valid);
    }

    constexpr auto minimum_progress{ 3ms };
    const auto progress_deadline{ std::chrono::steady_clock::now() + 100ms };
    auto after{ std::chrono::system_clock::now() };
    while (after - before < minimum_progress && std::chrono::steady_clock::now() < progress_deadline) {
        std::this_thread::sleep_for(1ms);
        after = std::chrono::system_clock::now();
    }

    EXPECT_GT(after, before);
    EXPECT_GE(after - before, minimum_progress);
    ASSERT_TRUE(timer->start().has_value());
}
