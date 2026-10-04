#pragma once

#include "MotionProfile.hpp"
#include "hal/drivers/itf/IStepGenerator.hpp"

namespace motion
{
    // A bounded tail: one active command plus one blended successor. Pulse
    // numbers remain absolute within the current hardware run after a splice.
    class Sequence final : public hal::step::Sequence
    {
      public:
        struct Part
        {
            Profile profile;
            hal::step::PulseCount begin{}, end{};
        };
        Sequence(double step, const Part& first, const std::optional<Part>& second = {});
        hal::step::PulseCount count() const noexcept override
        {
            return m_second ? m_second->end : m_first.end;
        }
        std::chrono::nanoseconds maximumPeriod() const noexcept override { return m_maximum; }
        const Part& first() const noexcept { return m_first; }
        const std::optional<Part>& second() const noexcept { return m_second; }
        Profile::State atPulse(hal::step::PulseCount pulse) const noexcept;
        double timeAtPulse(hal::step::PulseCount pulse, double hint = 0.0) const noexcept;
        hal::util::Result<hal::step::Timing> timing(hal::step::PulseCount pulse) const noexcept override;
        hal::util::Result<> generate(hal::step::PulseCount first,
                                     std::span<hal::step::Timing> target) const noexcept override;

      private:
        static hal::util::Result<hal::step::Timing> makeTiming(double seconds) noexcept;
        double m_step;
        Part m_first;
        std::optional<Part> m_second;
        std::chrono::nanoseconds m_maximum;
    };
}
