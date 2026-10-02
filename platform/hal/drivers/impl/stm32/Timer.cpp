#include "Timer.hpp"

#include "hal/drivers/common.hpp"
#include "hal/stm32/InterruptGuard.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <utility>

namespace hal
{
    namespace
    {
        constexpr std::uint64_t NANOSECONDS_PER_SECOND{ 1'000'000'000ULL };
        constexpr std::uint32_t TIMER_INTERRUPT_PRIORITY{ 5U };
        constexpr ITimer::Tick MAXIMUM_PRESCALER{ TIM_PSC_PSC };
    }

    Timer::Timer(Configuration configuration)
      : m_handle{ configuration.handle }
      , m_timerInputHz{ configuration.input_frequency_hz }
      , m_interrupt{ configuration.interrupt }
    {
        if (std::ranges::any_of(s_registry, [&](const Timer* timer) {
            return timer != nullptr && &timer->m_handle == &configuration.handle;
        })) {
            std::terminate();
        }

        auto* const slot{ std::ranges::find(s_registry, nullptr) };
        if (slot == s_registry.end()) {
            std::terminate();
        }
        *slot = this;
    }

    Timer::~Timer()
    {
        static_cast<void>(HAL_TIM_Base_Stop_IT(&m_handle));
        HAL_NVIC_DisableIRQ(m_interrupt);
        HAL_NVIC_ClearPendingIRQ(m_interrupt);

        const auto slot{ std::ranges::find(s_registry, this) };
        if (slot != s_registry.end()) {
            *slot = nullptr;
        }
    }

    auto Timer::start() noexcept -> util::Result<>
    {
        if (!isRunning()) {
            const auto result{ make_result(HAL_TIM_Base_Start(&m_handle)) };
            if (!result) {
                return result;
            }
        }

        __HAL_TIM_DISABLE_IT(&m_handle, TIM_IT_UPDATE);
        HAL_NVIC_DisableIRQ(m_interrupt);
        HAL_NVIC_ClearPendingIRQ(m_interrupt);
        markStateChange();
        return {};
    }

    auto Timer::stop() noexcept -> util::Result<>
    {
        const bool restore_update_interrupt{ __HAL_TIM_GET_IT_SOURCE(&m_handle, TIM_IT_UPDATE) != RESET };
        const bool restore_nvic_interrupt{ NVIC_GetEnableIRQ(m_interrupt) != 0U };

        __HAL_TIM_DISABLE_IT(&m_handle, TIM_IT_UPDATE);
        HAL_NVIC_DisableIRQ(m_interrupt);

        const auto result{ make_result(HAL_TIM_Base_Stop(&m_handle)) };
        if (!result) {
            if (restore_update_interrupt) {
                __HAL_TIM_ENABLE_IT(&m_handle, TIM_IT_UPDATE);
            }
            if (restore_nvic_interrupt) {
                HAL_NVIC_EnableIRQ(m_interrupt);
            }
            return result;
        }

        HAL_NVIC_ClearPendingIRQ(m_interrupt);
        markStateChange();
        return {};
    }

    auto Timer::startIt() noexcept -> util::Result<>
    {
        const bool update_interrupt_enabled{ __HAL_TIM_GET_IT_SOURCE(&m_handle, TIM_IT_UPDATE) != RESET };
        if (isRunning()) {
            if (!update_interrupt_enabled) {
                HAL_NVIC_DisableIRQ(m_interrupt);
                __HAL_TIM_CLEAR_FLAG(&m_handle, TIM_FLAG_UPDATE);
                HAL_NVIC_ClearPendingIRQ(m_interrupt);
                __HAL_TIM_ENABLE_IT(&m_handle, TIM_IT_UPDATE);
            }

            HAL_NVIC_SetPriority(m_interrupt, TIMER_INTERRUPT_PRIORITY, 0U);
            markStateChange();
            HAL_NVIC_EnableIRQ(m_interrupt);
            return {};
        }

        const bool restore_nvic_interrupt{ NVIC_GetEnableIRQ(m_interrupt) != 0U };
        HAL_NVIC_DisableIRQ(m_interrupt);
        __HAL_TIM_CLEAR_FLAG(&m_handle, TIM_FLAG_UPDATE);
        HAL_NVIC_ClearPendingIRQ(m_interrupt);

        const auto result{ make_result(HAL_TIM_Base_Start_IT(&m_handle)) };
        if (!result) {
            if (update_interrupt_enabled) {
                __HAL_TIM_ENABLE_IT(&m_handle, TIM_IT_UPDATE);
            }
            if (restore_nvic_interrupt) {
                HAL_NVIC_EnableIRQ(m_interrupt);
            }
            return result;
        }

        HAL_NVIC_SetPriority(m_interrupt, TIMER_INTERRUPT_PRIORITY, 0U);
        markStateChange();
        HAL_NVIC_EnableIRQ(m_interrupt);
        return {};
    }

    auto Timer::stopIt() noexcept -> util::Result<> { return stop(); }

    auto Timer::getCounter() const noexcept -> Tick
    {
        return static_cast<Tick>(__HAL_TIM_GET_COUNTER(&m_handle));
    }

    auto Timer::setCounter(Tick value) noexcept -> void
    {
        markStateChange();
        __HAL_TIM_SET_COUNTER(&m_handle, value);
    }

    auto Timer::getAutoReload() const noexcept -> Tick
    {
        return static_cast<Tick>(__HAL_TIM_GET_AUTORELOAD(&m_handle));
    }

    auto Timer::setAutoReload(Tick value) noexcept -> void
    {
        markStateChange();
        __HAL_TIM_SET_AUTORELOAD(&m_handle, value);
    }

    auto Timer::getPrescaler() const noexcept -> Tick { return static_cast<Tick>(m_handle.Instance->PSC); }

    auto Timer::setPrescaler(Tick value) noexcept -> void
    {
        // PSC is 16-bit even on TIM2's 32-bit counter. Saturate instead of
        // allowing the peripheral to silently wrap an out-of-range request.
        value = std::min(value, MAXIMUM_PRESCALER);
        const bool restore_update_interrupt{ __HAL_TIM_GET_IT_SOURCE(&m_handle, TIM_IT_UPDATE) != RESET };
        const bool restore_nvic_interrupt{ NVIC_GetEnableIRQ(m_interrupt) != 0U };

        HAL_NVIC_DisableIRQ(m_interrupt);
        __HAL_TIM_DISABLE_IT(&m_handle, TIM_IT_UPDATE);
        const bool update_was_pending{ __HAL_TIM_GET_FLAG(&m_handle, TIM_FLAG_UPDATE) != RESET };
        const bool interrupt_was_pending{ NVIC_GetPendingIRQ(m_interrupt) != 0U };
        const Tick counter{ getCounter() };

        __HAL_TIM_SET_PRESCALER(&m_handle, value);
        m_handle.Instance->EGR = TIM_EGR_UG;
        // UG latches the buffered prescaler but also reinitializes CNT. Preserve
        // the elapsed ticks, matching the Linux timer's setPrescaler semantics.
        __HAL_TIM_SET_COUNTER(&m_handle, counter);

        if (!update_was_pending) {
            __HAL_TIM_CLEAR_FLAG(&m_handle, TIM_FLAG_UPDATE);
        }
        if (!interrupt_was_pending) {
            HAL_NVIC_ClearPendingIRQ(m_interrupt);
        }
        if (restore_update_interrupt) {
            __HAL_TIM_ENABLE_IT(&m_handle, TIM_IT_UPDATE);
        }
        markStateChange();
        if (restore_nvic_interrupt) {
            HAL_NVIC_EnableIRQ(m_interrupt);
        }
    }

    auto Timer::forceUpdateEvent() noexcept -> void
    {
        markStateChange();
        m_handle.Instance->EGR = TIM_EGR_UG;
    }

    auto Timer::reset() noexcept -> void { setCounter(0U); }

    auto Timer::getInputFrequencyHz() const noexcept -> std::uint32_t { return m_timerInputHz; }

    auto Timer::getTickFrequencyHz() const noexcept -> std::uint32_t
    {
        const std::uint64_t divisor{ static_cast<std::uint64_t>(getPrescaler()) + 1U };
        return static_cast<std::uint32_t>(m_timerInputHz / divisor);
    }

    auto Timer::isRunning() const noexcept -> bool
    {
        return m_handle.Instance != nullptr && (m_handle.Instance->CR1 & TIM_CR1_CEN) != 0U;
    }

    auto Timer::getStateVersion() const noexcept -> std::uint32_t
    {
        return m_stateVersion.load(std::memory_order_relaxed);
    }

    auto Timer::durationToTicksImpl(std::chrono::nanoseconds duration) const noexcept -> Tick
    {
        const auto nanoseconds{ duration.count() };
        const std::uint64_t frequency{ getTickFrequencyHz() };
        if (nanoseconds <= 0 || frequency == 0U) {
            return 0U;
        }

        const auto unsigned_nanoseconds{ static_cast<std::uint64_t>(nanoseconds) };
        const std::uint64_t seconds{ unsigned_nanoseconds / NANOSECONDS_PER_SECOND };
        const std::uint64_t remainder{ unsigned_nanoseconds % NANOSECONDS_PER_SECOND };
        constexpr std::uint64_t maximum_tick{ std::numeric_limits<Tick>::max() };
        if (seconds > maximum_tick / frequency) {
            return std::numeric_limits<Tick>::max();
        }

        const std::uint64_t whole_ticks{ seconds * frequency };
        const std::uint64_t fractional_ticks{ (remainder * frequency + NANOSECONDS_PER_SECOND - 1U) /
                                              NANOSECONDS_PER_SECOND };
        if (fractional_ticks > maximum_tick - whole_ticks) {
            return std::numeric_limits<Tick>::max();
        }
        return static_cast<Tick>(whole_ticks + fractional_ticks);
    }

    auto Timer::setPeriodImpl(std::chrono::nanoseconds duration) noexcept -> void
    {
        const Tick ticks{ durationToTicks(duration) };
        setAutoReload(ticks == 0U ? 0U : ticks - 1U);
        forceUpdateEvent();
    }

    auto Timer::getElapsedTime() const noexcept -> std::chrono::nanoseconds
    {
        const std::uint64_t frequency{ getTickFrequencyHz() };
        if (frequency == 0U) {
            return std::chrono::nanoseconds::zero();
        }

        const std::uint64_t nanoseconds{ static_cast<std::uint64_t>(getCounter()) * NANOSECONDS_PER_SECOND /
                                         frequency };
        return std::chrono::nanoseconds{ static_cast<std::int64_t>(nanoseconds) };
    }

    auto Timer::setPeriodElapsedCallback(PeriodElapsedCallback callback) noexcept -> void
    {
        {
            // Masking TIM2 alone still allows another task to preempt this
            // setter. Protect the swap against both tasks and callbacks, then
            // release the old callback with normal interrupt state restored.
            const stm32::InterruptGuard interrupt_guard;
            m_periodElapsedCallback.swap(callback);
        }
    }

    auto Timer::markStateChange() noexcept -> void
    {
        static_cast<void>(m_stateVersion.fetch_add(1U, std::memory_order_relaxed));
    }

    auto Timer::dispatchPeriodElapsed(TIM_HandleTypeDef* handle) noexcept -> void
    {
        const auto timer{ std::ranges::find_if(s_registry, [&](const Timer* candidate) {
            return candidate != nullptr && &candidate->m_handle == handle;
        }) };
        if (timer != s_registry.end() && (*timer)->m_periodElapsedCallback) {
            (*timer)->m_periodElapsedCallback();
        }
    }
}

extern "C" void TIM2_IRQHandler()
{
    if (__HAL_TIM_GET_FLAG(&htim2, TIM_FLAG_UPDATE) != RESET &&
        __HAL_TIM_GET_IT_SOURCE(&htim2, TIM_IT_UPDATE) != RESET) {
        __HAL_TIM_CLEAR_IT(&htim2, TIM_IT_UPDATE);
        hal::Timer::dispatchPeriodElapsed(&htim2);
    }
}
