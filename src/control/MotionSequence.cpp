#include "MotionSequence.hpp"

#include <algorithm>
#include <cmath>

namespace motion
{
    Sequence::Sequence(double step, const Part& first, const std::optional<Part>& second)
      : m_step{ step }
      , m_first{ first }
      , m_second{ second }
    {
        auto bound{ m_first.profile.maximumStepInterval(step) };
        if (m_second)
            bound = std::max(bound, m_second->profile.maximumStepInterval(step));
        m_maximum = std::chrono::nanoseconds{ static_cast<std::int64_t>(
          std::ceil(std::clamp(bound * 1e9 + 200.0, 10000.0, 1e15))) };
    }

    Profile::State Sequence::atPulse(hal::step::PulseCount pulse) const noexcept
    {
        const auto& part{ m_second && pulse > m_first.end ? *m_second : m_first };
        return part.profile.at(part.profile.timeAt(static_cast<double>(pulse - part.begin) * m_step));
    }

    double Sequence::timeAtPulse(hal::step::PulseCount pulse, double hint) const noexcept
    {
        if (m_second && pulse > m_first.end)
            return m_first.profile.duration() +
                   m_second->profile.timeAt(static_cast<double>(pulse - m_second->begin) * m_step,
                                            hint - m_first.profile.duration());
        return m_first.profile.timeAt(static_cast<double>(pulse - m_first.begin) * m_step, hint);
    }

    hal::util::Result<hal::step::Timing> Sequence::timing(hal::step::PulseCount pulse) const noexcept
    {
        if (pulse >= count() || pulse + 1U < m_first.begin)
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        const auto end{ pulse + 1U };
        const auto seconds{ end == count() ? Sequence::timeAtPulse(end) - timeAtPulse(end - 1U)
                                           : timeAtPulse(end + 1U) - timeAtPulse(end) };
        return Sequence::makeTiming(seconds);
    }

    hal::util::Result<> Sequence::generate(hal::step::PulseCount first,
                                           std::span<hal::step::Timing> target) const noexcept
    {
        if (first >= count() || target.size() > count() - first || first + 1U < m_first.begin)
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        auto previous{ Sequence::timeAtPulse(first + 1U) };
        for (auto& entry : target) {
            const auto pulse{ ++first };
            const auto next{ pulse < count() ? timeAtPulse(pulse + 1U, previous) : previous };
            const auto seconds{ pulse < count() ? next - previous : previous - timeAtPulse(pulse - 1U) };
            const auto value{ Sequence::makeTiming(seconds) };
            if (!value)
                return std::unexpected(value.error());
            entry = *value;
            previous = next;
        }
        return {};
    }

    hal::util::Result<hal::step::Timing> Sequence::makeTiming(double seconds) noexcept
    {
        if (!std::isfinite(seconds) || seconds <= 0.0 || seconds > 53.6870911)
            return std::unexpected(std::make_error_code(std::errc::result_out_of_range));
        return hal::step::Timing{ std::chrono::nanoseconds{ std::max<std::int64_t>(
                                    10000, static_cast<std::int64_t>(std::ceil(seconds * 1e9 - 0.001))) },
                                  std::chrono::microseconds{ 5 } };
    }
}
