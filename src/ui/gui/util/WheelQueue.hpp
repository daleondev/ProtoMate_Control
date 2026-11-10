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
            if (report.wheel == 0) {
                return true;
            }
            if (m_count == m_events.size()) {
                clear();
                ++m_dropped;
                return false;
            }
            m_events[(m_head + m_count++) % m_events.size()] = {
                .at = now, .delta = report.wheel, .x = report.pointer_x, .y = report.pointer_y
            };
            return true;
        }
        void consume(hal::device::TouchReport& report, Clock::time_point now)
        {
            report.wheel = 0;
            while (m_count) {
                const auto e{ m_events[m_head] };
                m_head = (m_head + 1) % m_events.size();
                --m_count;
                if (now - e.at > MAX_AGE) {
                    continue;
                }
                report.wheel = e.delta;
                report.pointer_x = e.x;
                report.pointer_y = e.y;
                return;
            }
        }
        void clear() { m_head = m_count = 0; }
        unsigned dropped() const { return m_dropped; }

      private:
        static constexpr unsigned CAPACITY{ 8 };
        static constexpr auto MAX_AGE{ std::chrono::milliseconds{ 100 } };
        struct Event
        {
            Clock::time_point at;
            float delta{};
            std::uint16_t x{}, y{};
        };
        std::array<Event, CAPACITY> m_events{};
        unsigned m_head{}, m_count{}, m_dropped{};
    };
}
