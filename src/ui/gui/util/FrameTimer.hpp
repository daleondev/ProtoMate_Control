#pragma once
#include "hal/drivers/itf/ITimer.hpp"
#include <chrono>
#include <memory>
namespace ui::gui::util
{
    // Combine the hardware counter's sub-tick precision with ThreadX's
    // monotonic clock to disambiguate any number of timer wraps.
    class FrameTimer
    {
      public:
        explicit FrameTimer(std::shared_ptr<hal::ITimer> timer)
          : m_timer(std::move(timer))
        {
        }
        void start()
        {
            m_coarse = std::chrono::steady_clock::now();
            m_ticks = m_timer->getCounter();
            m_version = m_timer->getStateVersion();
        }
        std::chrono::microseconds elapsed() const
        {
            const auto end = m_timer->getCounter();
            const auto coarse = std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - m_coarse);
            const auto frequency = m_timer->getTickFrequencyHz();
            const auto period = std::uint64_t(m_timer->getAutoReload()) + 1;
            if (!frequency || m_timer->getStateVersion() != m_version || period * 1000 / frequency < 4) {
                return coarse;
            }
            const auto delta = (std::uint64_t(end) + period - m_ticks) % period;
            const auto approximate = std::uint64_t(coarse.count()) * frequency / 1000000;
            const auto wraps =
              approximate + period / 2 > delta ? (approximate + period / 2 - delta) / period : 0;
            return std::chrono::microseconds{ (wraps * period + delta) * 1000000 / frequency };
        }

      private:
        std::shared_ptr<hal::ITimer> m_timer;
        std::chrono::steady_clock::time_point m_coarse;
        hal::ITimer::Tick m_ticks{};
        std::uint32_t m_version{};
    };
}
