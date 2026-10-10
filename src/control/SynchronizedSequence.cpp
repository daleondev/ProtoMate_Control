#include "SynchronizedSequence.hpp"
#include "MotionLimits.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace motion
{
    namespace
    {
        using namespace std::chrono_literals;
        constexpr auto TICK_NS{ limits::TIMER_TICK.count() };
        constexpr auto MAXIMUM_PERIOD{ limits::MAX_PERIOD };

        hal::util::Result<hal::step::Timing> interval(std::int64_t ticks) noexcept
        {
            const auto period{ std::chrono::nanoseconds{ ticks * TICK_NS } };
            if (period < limits::MIN_PERIOD || period > MAXIMUM_PERIOD)
                return std::unexpected(std::make_error_code(std::errc::result_out_of_range));
            return hal::step::Timing{ .period = period, .high_time = limits::HIGH_TIME };
        }
    }

    SynchronizedSequence::SynchronizedSequence(Profile profile, hal::step::PulseCount count)
      : m_profile{ profile }
      , m_count{ count }
    {
        if (count == 0U ||
            count >= static_cast<hal::step::PulseCount>(static_cast<double>(motion::limits::MAX_PULSES)) ||
            profile.distance() != 1.0 || !std::isfinite(profile.duration()) || profile.duration() <= 0.0 ||
            profile.duration() > limits::MAX_PROFILE_SECONDS)
            throw std::invalid_argument("invalid synchronized profile");
        const auto bound{ profile.maximumStepInterval(1.0 / static_cast<double>(count)) };
        if (!std::isfinite(bound) || bound > limits::SYNCHRONIZED_HORIZON_SECONDS)
            throw std::invalid_argument("synchronized move exceeds the timer's slow-motion horizon");
        m_maximum = std::chrono::nanoseconds{ std::max<std::int64_t>(
          limits::MIN_PERIOD.count(),
          static_cast<std::int64_t>(
            std::ceil((bound * limits::TICKS_PER_SECOND) +
                      static_cast<double>(limits::BOUND_ROUNDING_MARGIN / limits::TIMER_TICK))) *
            TICK_NS) };
    }

    std::int64_t SynchronizedSequence::tickAt(hal::step::PulseCount pulse) const noexcept
    {
        // Use the same inversion path for random access and every buffer size;
        // a caller-dependent hint can round a near-boundary result differently.
        const auto time{ pulse == m_count
                           ? m_profile.duration()
                           : m_profile.timeAt(static_cast<double>(pulse) / static_cast<double>(m_count)) };
        return static_cast<std::int64_t>(std::ceil(time * limits::TICKS_PER_SECOND));
    }

    std::chrono::nanoseconds SynchronizedSequence::firstDelay() const noexcept
    {
        return std::chrono::nanoseconds{ tickAt(1U) * TICK_NS };
    }

    hal::util::Result<hal::step::Timing> SynchronizedSequence::timing(
      hal::step::PulseCount pulse) const noexcept
    {
        hal::step::Timing value;
        if (auto result{ generate(pulse, std::span{ &value, 1U }) }; !result)
            return std::unexpected(result.error());
        return value;
    }

    hal::util::Result<> SynchronizedSequence::generate(hal::step::PulseCount first,
                                                       std::span<hal::step::Timing> target) const noexcept
    {
        if (first >= m_count || target.size() > m_count - first)
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        auto previous{ tickAt(first + 1U) };
        for (auto& entry : target) {
            const auto pulse{ ++first };
            auto next{ previous };
            std::int64_t ticks{};
            if (pulse < m_count) {
                next = tickAt(pulse + 1U);
                ticks = next - previous;
            }
            else {
                // No next rising edge, but the HAL reports this period while
                // the last pulse is high. Keep its velocity estimate meaningful.
                ticks = previous - tickAt(pulse - 1U);
            }
            const auto value{ interval(ticks) };
            if (!value)
                return std::unexpected(value.error());
            entry = *value;
            previous = next;
        }
        return {};
    }
}
