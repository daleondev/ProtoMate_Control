#pragma once

#include "ProfileCrossing.hpp"
#include "hal/drivers/itf/IStepGenerator.hpp"

namespace io::motion
{
    // One finite monotone move, optionally replaced by a C2-continuous local
    // brake at a time boundary. Coordinates are microsteps, time is seconds.
    // Immutable after construction; ISR evaluation has only stack-local cursors.
    class ProfileSchedule final : public hal::step::TimedSequence
    {
      public:
        // Shared once per axis. Renewed banks retain this immutable source;
        // only the stopping continuation is stored separately in each bank.
        struct Nominal final
        {
            explicit Nominal(const ::motion::Profile& value, std::uint32_t half_pulses = 64);
            Nominal(const Nominal&) = delete;
            Nominal& operator=(const Nominal&) = delete;
            ::motion::Profile profile;
            ProfileCrossing crossing;
            unsigned count{};
            std::uint32_t half_pulses;
            std::uint32_t first{}, end{};
            std::chrono::nanoseconds maximum{};
            bool valid{};
        };
        using Source = std::shared_ptr<const Nominal>;
        explicit ProfileSchedule(const ::motion::Profile& profile);
        explicit ProfileSchedule(Source nominal);
        ProfileSchedule(Source nominal, std::uint32_t cut, ::motion::Profile::Limits limits);
        ProfileSchedule(const ProfileSchedule& nominal, std::uint32_t cut, ::motion::Profile::Limits limits);
        ProfileSchedule(const ProfileSchedule&) = delete;
        ProfileSchedule& operator=(const ProfileSchedule&) = delete;
        bool valid() const noexcept { return m_valid; }
        std::uint32_t firstTick() const noexcept { return m_first; }
        std::uint32_t endTick() const noexcept { return m_end; }
        std::uint32_t cutTick() const noexcept { return m_cut; }
        std::uint32_t initialOffset() const noexcept override;
        const ::motion::Profile& profile() const noexcept { return m_source->profile; }
        std::optional<std::int64_t> commonPrefix(const hal::step::Sequence& previous) const noexcept override;
        hal::step::PulseCount count() const noexcept override { return m_count; }
        std::uint32_t bufferPulses() const noexcept override { return m_source ? m_source->half_pulses : 64; }
        std::chrono::nanoseconds maximumPeriod() const noexcept override;
        std::optional<std::int64_t> endTickOffset() const noexcept override
        {
            return std::int64_t(m_end) - m_first + 50;
        }
        hal::util::Result<std::uint32_t> risingTick(hal::step::PulseCount pulse) const noexcept override;
        hal::util::Result<hal::step::Timing> timing(hal::step::PulseCount pulse) const noexcept override;
        hal::util::Result<> generate(hal::step::PulseCount first,
                                     std::span<hal::step::Timing> target) const noexcept override;

      private:
        std::uint32_t absoluteTick(unsigned pulse,
                                   ProfileCrossing::Cursor& nominal,
                                   ProfileCrossing::Cursor& brake) const noexcept;
        Source m_source;
        std::optional<::motion::Profile> m_brake;
        std::optional<ProfileCrossing> m_braking;
        unsigned m_count{}, m_prefix{};
        std::uint32_t m_first{}, m_cut{}, m_end{};
        std::chrono::nanoseconds m_maximum{};
        bool m_valid{};
    };
}
