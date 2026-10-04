#pragma once

#include "MotionProfile.hpp"
#include "hal/drivers/itf/IStepGenerator.hpp"

namespace motion
{
    // Each axis samples the SAME normalized 0..1 profile at k/pulse_count.
    // Absolute pulse times are rounded to timer ticks before taking intervals,
    // so different pulse counts cannot accumulate different rounding drift.
    class SynchronizedSequence final : public hal::step::Sequence
    {
      public:
        SynchronizedSequence(Profile profile, hal::step::PulseCount count);
        hal::step::PulseCount count() const noexcept override { return m_count; }
        std::chrono::nanoseconds maximumPeriod() const noexcept override { return m_maximum; }
        std::chrono::nanoseconds firstDelay() const noexcept;
        hal::util::Result<hal::step::Timing> timing(hal::step::PulseCount pulse) const noexcept override;
        hal::util::Result<> generate(hal::step::PulseCount first,
                                     std::span<hal::step::Timing> target) const noexcept override;

      private:
        std::int64_t tickAt(hal::step::PulseCount pulse) const noexcept;
        Profile m_profile;
        hal::step::PulseCount m_count;
        std::chrono::nanoseconds m_maximum;
    };
}
