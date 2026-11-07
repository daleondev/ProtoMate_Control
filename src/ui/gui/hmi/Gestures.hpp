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
            const auto allowed = [&](int x, int y) {
                return inside(x, y) && std::none_of(exclusions.begin(), exclusions.end(), [&](const auto& r) {
                    return r.contains(x, y);
                });
            };
            Result result;
            unsigned count{};
            std::array<unsigned, 5> active{};
            for (unsigned i = 0; i < report.points.size(); ++i) {
                const auto& p = report.points[i];
                if (p.active) {
                    active[count++] = i;
                    if (!m_previous[i])
                        m_eligible[i] = allowed(p.x, p.y) && visible;
                }
                else
                    m_eligible[i] = false;
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
                result.captured = true;
                if (count != 2 || !m_eligible[active[0]] || !m_eligible[active[1]]) {
                    cancel();
                    return result;
                }
                const auto& a = report.points[active[0]];
                const auto& b = report.points[active[1]];
                const float distance = std::hypot(float(a.x) - b.x, float(a.y) - b.y);
                if (distance < 12) {
                    m_distance = 0;
                    m_pinching = true;
                    return result;
                }
                if (m_pinching && m_distance > 0 && std::abs(distance - m_distance) >= 2) {
                    result.scale = m_distance / distance;
                    m_distance = distance;
                }
                if (!m_pinching || !m_distance)
                    m_distance = distance;
                m_pinching = true;
            }
            else if (m_pinching) {
                cancel();
                result.captured = true;
            }
            if (!result.captured && report.wheel && allowed(report.pointerX, report.pointerY))
                result.scale = std::exp(-.12f * std::clamp(report.wheel, -20.f, 20.f));
            return result;
        }
        static bool inside(int x, int y)
        {
            return x >= theme::VIEW_X && x < theme::VIEW_X + theme::VIEW_WIDTH && y >= theme::VIEW_Y &&
                   y < theme::VIEW_Y + theme::VIEW_HEIGHT;
        }

      private:
        std::array<bool, 5> m_previous{}, m_eligible{};
        float m_distance{};
        bool m_pinching{}, m_locked{};
    };
}
