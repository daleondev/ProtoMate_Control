#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <utility>

namespace hal::detail
{
    // One owner per timer: period/prescaler and internal compare channels are shared.
    // TIM2 (step engine), TIM5 (runtime) and TIM6 (HAL tick) deliberately cannot be leased here.
    class TimerLease final
    {
      public:
        explicit TimerLease(std::uint8_t timer) noexcept
        {
            if (timer != 1U && timer != 3U && timer != 4U && timer != 8U) {
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
        std::uint8_t m_timer{};
        inline static std::array<std::atomic_bool, 9U> s_claimed{};
    };
}
