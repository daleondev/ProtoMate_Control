#pragma once

#include "hal/drivers/itf/IPwmOutput.hpp"

#include <limits>

namespace hal::util
{
    // One hardware pulse may be in flight. The backend must not rearm until
    // finishPulse() allows it, so interrupt latency cannot create extra edges.
    class PulseTrain
    {
      public:
        using Count = IPwmOutput::PulseCount;

        auto start(std::optional<Count> limit) noexcept -> void
        {
            m_count = 0U;
            m_limit = limit.value_or(std::numeric_limits<Count>::max());
            m_active = m_limit != 0U;
            m_risen = false;
        }
        [[nodiscard]] auto rise() noexcept -> bool
        {
            if (!m_active || m_risen) {
                return false;
            }
            ++m_count;
            m_risen = true;
            return true;
        }
        [[nodiscard]] auto finishPulse() noexcept -> bool
        {
            if (!m_active || !m_risen) {
                return false;
            }
            m_risen = false;
            m_active = m_count < m_limit;
            return m_active;
        }
        auto stop() noexcept -> void { m_active = false; }
        [[nodiscard]] auto count(bool pending_rise = false) const noexcept -> Count
        {
            return m_count + static_cast<Count>(pending_rise && m_active && !m_risen);
        }

      private:
        Count m_count{};
        Count m_limit{};
        bool m_active{};
        bool m_risen{};
    };
}
