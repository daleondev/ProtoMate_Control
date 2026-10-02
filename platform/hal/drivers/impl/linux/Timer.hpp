#pragma once

#include "hal/drivers/itf/ITimer.hpp"

#include <pthread.h>

#include <chrono>
#include <cstdint>
#include <memory>

namespace hal
{
    class Timer final : public ITimer
    {
      public:
        struct Configuration
        {
            std::uint32_t input_frequency_hz;
            Tick prescaler;
            Tick auto_reload;
        };

        explicit Timer(Configuration configuration) noexcept;
        ~Timer() override;

        Timer(const Timer&) = delete;
        Timer& operator=(const Timer&) = delete;
        Timer(Timer&&) = delete;
        Timer& operator=(Timer&&) = delete;

        [[nodiscard]] auto start() noexcept -> util::Result<> override;
        [[nodiscard]] auto stop() noexcept -> util::Result<> override;

        [[nodiscard]] auto startIt() noexcept -> util::Result<> override;
        [[nodiscard]] auto stopIt() noexcept -> util::Result<> override;

        [[nodiscard]] auto getCounter() const noexcept -> Tick override;
        auto setCounter(Tick value) noexcept -> void override;

        [[nodiscard]] auto getAutoReload() const noexcept -> Tick override;
        auto setAutoReload(Tick value) noexcept -> void override;

        [[nodiscard]] auto getPrescaler() const noexcept -> Tick override;
        auto setPrescaler(Tick value) noexcept -> void override;

        auto forceUpdateEvent() noexcept -> void override;
        auto reset() noexcept -> void override;

        [[nodiscard]] auto getInputFrequencyHz() const noexcept -> std::uint32_t override;
        [[nodiscard]] auto getTickFrequencyHz() const noexcept -> std::uint32_t override;
        [[nodiscard]] auto isRunning() const noexcept -> bool override;
        [[nodiscard]] auto getStateVersion() const noexcept -> std::uint32_t override;

        [[nodiscard]] auto getElapsedTime() const noexcept -> std::chrono::nanoseconds override;

        auto setPeriodElapsedCallback(PeriodElapsedCallback callback) noexcept -> void override;

      private:
        [[nodiscard]] auto durationToTicksImpl(std::chrono::nanoseconds duration) const noexcept
          -> Tick override;
        auto setPeriodImpl(std::chrono::nanoseconds duration) noexcept -> void override;

        [[nodiscard]] auto counterAt(std::uint64_t monotonic_nanoseconds) const noexcept -> Tick;
        [[nodiscard]] auto durationToTicksUnlocked(std::chrono::nanoseconds duration) const noexcept -> Tick;
        [[nodiscard]] auto getTickFrequencyHzUnlocked() const noexcept -> std::uint32_t;
        [[nodiscard]] auto nanosecondsUntilOverflowUnlocked(
          std::uint64_t monotonic_nanoseconds) const noexcept -> std::uint64_t;
        auto captureCounter(std::uint64_t monotonic_nanoseconds) noexcept -> void;
        auto notifyWorker() noexcept -> void;
        auto workerLoop() noexcept -> void;
        static auto workerEntry(void* context) noexcept -> void*;

        std::uint32_t m_timerInputHz;
        Tick m_prescaler;
        Tick m_autoReload;
        Tick m_counter{};
        std::uint64_t m_startedAtNanoseconds{};
        bool m_running{};
        bool m_interruptEnabled{};
        bool m_shuttingDown{};
        std::uint32_t m_stateVersion{};
        mutable pthread_mutex_t m_mutex{};
        pthread_cond_t m_condition{};
        pthread_t m_worker{};
        std::shared_ptr<PeriodElapsedCallback> m_periodElapsedCallback;
    };
}
