#pragma once

#include "hal/drivers/itf/ITimer.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <utility>

namespace hal::util
{
    // One owner per timer: period/prescaler and internal compare channels are shared.
    // TIM2/TIM7 (step engine), TIM5 (runtime) and TIM6 (HAL tick) cannot be leased here.
    class TimerLease final
    {
      public:
        explicit TimerLease(timer::Peripheral peripheral) noexcept
        {
            const auto timer{ std::to_underlying(peripheral) };
            if (peripheral != timer::Peripheral::Tim1 && peripheral != timer::Peripheral::Tim3 &&
                peripheral != timer::Peripheral::Tim4 && peripheral != timer::Peripheral::Tim8) {
                return;
            }
            bool expected{ false };
            if (s_claimed[timer].compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                m_timer = timer;
            }
        }
        ~TimerLease()
        {
            if (m_timer != 0U) {
                s_claimed[m_timer].store(false, std::memory_order_release);
            }
        }
        TimerLease(TimerLease&& other) noexcept
          : m_timer{ std::exchange(other.m_timer, 0U) }
        {
        }
        TimerLease(const TimerLease&) = delete;
        TimerLease& operator=(const TimerLease&) = delete;
        TimerLease& operator=(TimerLease&&) = delete;
        explicit operator bool() const noexcept { return m_timer != 0U; }

      private:
        static constexpr auto TIMER_LEASE_SLOTS{ 9U };

        std::uint8_t m_timer{};
        inline static std::array<std::atomic_bool, TIMER_LEASE_SLOTS> s_claimed{};
    };
}
