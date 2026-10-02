#pragma once

#include "hal/drivers/itf/ITimer.hpp"
#include "hal/hal.hpp"

#include <array>
#include <atomic>
#include <cstddef>

namespace hal
{
    class Timer final : public ITimer
    {
      public:
        struct Configuration
        {
            TIM_HandleTypeDef& handle;
            std::uint32_t input_frequency_hz;
            IRQn_Type interrupt;
        };

        explicit Timer(Configuration configuration);
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

        static auto dispatchPeriodElapsed(TIM_HandleTypeDef* handle) noexcept -> void;

        auto setPeriodElapsedCallback(PeriodElapsedCallback callback) noexcept -> void override;

      private:
        [[nodiscard]] auto durationToTicksImpl(std::chrono::nanoseconds duration) const noexcept
          -> Tick override;

        auto setPeriodImpl(std::chrono::nanoseconds duration) noexcept -> void override;

        auto markStateChange() noexcept -> void;

        TIM_HandleTypeDef& m_handle;
        std::uint32_t m_timerInputHz;
        IRQn_Type m_interrupt;
        PeriodElapsedCallback m_periodElapsedCallback;
        std::atomic<std::uint32_t> m_stateVersion{};

        static constexpr std::size_t MAX_TIMER_INSTANCES{ 4U };
        inline static std::array<Timer*, MAX_TIMER_INSTANCES> s_registry{};
    };
}
