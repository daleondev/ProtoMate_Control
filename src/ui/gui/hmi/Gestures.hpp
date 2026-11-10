#pragma once
#include "hal/devices/itf/ITouch.hpp"
#include "ui/gui/frontend/Theme.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
namespace ui::gui::hmi
{
    // GUI-thread recognizer. Acquisition separately cancels held motion on multi-touch.
    class Gestures
    {
      public:
        struct Exclusion
        {
            int x{}, y{}, width{}, height{};
            bool contains(int px, int py) const
            {
                return px >= x && py >= y && px < x + width && py < y + height;
            }
        };
        struct Result
        {
            bool captured{};
            float scale{ 1 };
        };
        void cancel()
        {
            m_locked = true;
            m_pinching = false;
            m_distance = 0;
            m_previous = {};
            m_eligible = {};
        }
        Result update(const hal::device::TouchReport& report,
                      bool valid,
                      bool visible,
                      std::span<const Exclusion> exclusions = {})
        {
            const auto allowed{ [&](int x, int y) {
                return inside(x, y) &&
                       std::ranges::none_of(exclusions, [&](const auto& r) { return r.contains(x, y); });
            } };
            Result result{};
            unsigned count{};
            std::array<unsigned, CONTACTS> active{};
            for (unsigned i{ 0 }; i < report.points.size(); ++i) {
                const auto& p{ report.points[i] };
                if (p.active) {
                    active[count++] = i;
                    if (!m_previous[i]) {
                        m_eligible[i] = allowed(p.x, p.y) && visible;
                    }
                }
                else {
                    m_eligible[i] = false;
                }
                m_previous[i] = p.active;
            }
            if (!valid || !visible || report.cancelled) {
                cancel();
                result.captured = true;
                return result;
            }
            if (!count) {
                m_locked = false;
                m_pinching = false;
                m_distance = 0;
            }
            if (m_locked) {
                result.captured = true;
                return result;
            }
            if (count >= 2) {
                result = pinch(report, std::span(active).first(count));
            }
            else if (m_pinching) {
                cancel();
                result.captured = true;
            }
            if (!result.captured && report.wheel != 0 && allowed(report.pointer_x, report.pointer_y)) {
                result.scale =
                  std::exp(-WHEEL_SCALE * std::clamp(report.wheel, -MAX_WHEEL_DELTA, MAX_WHEEL_DELTA));
            }
            return result;
        }
        static bool inside(int x, int y)
        {
            return x >= theme::VIEW_X && x < theme::VIEW_X + theme::VIEW_WIDTH && y >= theme::VIEW_Y &&
                   y < theme::VIEW_Y + theme::VIEW_HEIGHT;
        }

      private:
        static constexpr unsigned CONTACTS{ 5 };
        static constexpr float MIN_SEPARATION{ 12 }, JITTER_PIXELS{ 2 }, WHEEL_SCALE{ .12f },
          MAX_WHEEL_DELTA{ 20 };
        Result pinch(const hal::device::TouchReport& report, std::span<const unsigned> active)
        {
            Result result{ .captured = true };
            if (active.size() != 2 || !m_eligible[active[0]] || !m_eligible[active[1]]) {
                cancel();
                return result;
            }
            const auto& a{ report.points[active[0]] };
            const auto& b{ report.points[active[1]] };
            const float distance{ std::hypot(float(a.x) - float(b.x), float(a.y) - float(b.y)) };
            if (distance < MIN_SEPARATION) {
                m_distance = 0;
                m_pinching = true;
                return result;
            }
            if (m_pinching && m_distance > 0 && std::abs(distance - m_distance) >= JITTER_PIXELS) {
                result.scale = m_distance / distance;
                m_distance = distance;
            }
            if (!m_pinching || m_distance == 0) {
                m_distance = distance;
            }
            m_pinching = true;
            return result;
        }
        std::array<bool, CONTACTS> m_previous{}, m_eligible{};
        float m_distance{};
        bool m_pinching{}, m_locked{};
    };
}
