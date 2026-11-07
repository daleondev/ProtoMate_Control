#pragma once
#include "Robot.hpp"
#include <chrono>
#include <optional>

namespace ui::gui::scene3d
{
    // Change-driven policy shared by hardware and simulation. A new timestamp
    // alone is not a visual change. Refine once after motion settles, then idle.
    class DetailPolicy
    {
      public:
        using Clock = std::chrono::steady_clock;
        static constexpr auto SETTLE_TIME = std::chrono::milliseconds{ 500 };
        static constexpr unsigned INTERACTIVE_MAX_FPS = 10;

        void changed(Clock::time_point now)
        {
            m_changed = now;
            m_dirty = true;
        }
        void invalidate() { m_dirty = true; }
        std::optional<RobotDetail> request(Clock::time_point now) const
        {
            const auto detail =
              m_changed && now - *m_changed < SETTLE_TIME ? RobotDetail::Interactive : RobotDetail::Full;
            return m_dirty || detail != m_submitted ? std::optional{ detail } : std::nullopt;
        }
        void submitted(RobotDetail detail)
        {
            m_submitted = detail;
            m_dirty = false;
        }

      private:
        std::optional<Clock::time_point> m_changed;
        RobotDetail m_submitted{ RobotDetail::Full };
        bool m_dirty{ true };
    };
}
