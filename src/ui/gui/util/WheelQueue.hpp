#pragma once
#include "hal/devices/itf/ITouch.hpp"
#include <array>
#include <chrono>
namespace ui::gui::util
{
    // Protected by Controller's existing input mutex. Coordinates travel with
    // each delta, so coalescing cannot move a scroll from a card onto the scene.
    class WheelQueue
    {
      public:
        using Clock = std::chrono::steady_clock;
        bool push(const hal::device::TouchReport& report, Clock::time_point now)
        {
            if (report.cancelled) {
                clear();
                return true;
            }
            if (!report.wheel)
                return true;
            if (m_count == m_events.size()) {
                clear();
                ++m_dropped;
                return false;
            }
            m_events[(m_head + m_count++) % m_events.size()] = {
                now, report.wheel, report.pointerX, report.pointerY
            };
            return true;
        }
        void consume(hal::device::TouchReport& report, Clock::time_point now)
        {
            report.wheel = 0;
            while (m_count) {
                const auto e = m_events[m_head];
                m_head = (m_head + 1) % m_events.size();
                --m_count;
                if (now - e.at > std::chrono::milliseconds{ 100 })
                    continue;
                report.wheel = e.delta;
                report.pointerX = e.x;
                report.pointerY = e.y;
                return;
            }
        }
        void clear() { m_head = m_count = 0; }
        unsigned dropped() const { return m_dropped; }

      private:
        struct Event
        {
            Clock::time_point at;
            float delta{};
            std::uint16_t x{}, y{};
        };
        std::array<Event, 8> m_events{};
        unsigned m_head{}, m_count{}, m_dropped{};
    };
}
