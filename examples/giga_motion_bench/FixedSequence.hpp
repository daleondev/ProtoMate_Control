#pragma once
#include "FixedCrossing.hpp"
#include "hal/drivers/itf/IStepGenerator.hpp"

namespace motion_bench
{
    // Bench-only immutable sequence. Every DMA fill has its own tiny cursor;
    // no mutable interpolation state, allocations or checkpoint table in IRQs.
    class FixedSequence final : public hal::step::Sequence
    {
      public:
        explicit FixedSequence(const motion::Profile& profile)
          : m_profile(profile), m_program(m_profile), m_count(unsigned(std::floor(profile.distance())))
        {
            FixedCrossing::Cursor cursor;
            m_first = m_program.next(1, cursor);
            FixedCrossing::Cursor ending;
            const auto previous = m_count > 1 ? m_program.next(m_count - 1, ending) : 0;
            m_lastPeriod = m_program.next(m_count, ending) - previous;
            m_maximum = std::chrono::nanoseconds(std::uint64_t(std::ceil(profile.maximumStepInterval(1) * 1e7) + 4) * 100);
        }
        FixedSequence(const FixedSequence&) = delete;
        FixedSequence& operator=(const FixedSequence&) = delete;
        FixedSequence(FixedSequence&&) = delete;
        FixedSequence& operator=(FixedSequence&&) = delete;
        bool valid() const noexcept { return m_program.valid() && m_count && m_first; }
        std::uint32_t firstTick() const noexcept { return m_first; }
        hal::step::PulseCount count() const noexcept override { return m_count; }
        std::chrono::nanoseconds maximumPeriod() const noexcept override { return m_maximum; }
        hal::util::Result<hal::step::Timing> timing(hal::step::PulseCount first) const noexcept override
        {
            hal::step::Timing value;
            if (auto result = generate(first, std::span(&value, 1)); !result)
                return std::unexpected(result.error());
            return value;
        }
        hal::util::Result<> generate(hal::step::PulseCount first, std::span<hal::step::Timing> out) const noexcept override
        {
            if (!valid() || first >= m_count || out.size() > m_count - first)
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));
            FixedCrossing::Cursor cursor;
            auto previous = m_program.next(unsigned(first + 1), cursor);
            for (auto& timing : out) {
                ++first;
                const auto next = first < m_count ? m_program.next(unsigned(first + 1), cursor) : previous + m_lastPeriod;
                if (next < previous || next - previous < 400)
                    return std::unexpected(std::make_error_code(std::errc::result_out_of_range));
                timing = {std::chrono::nanoseconds(std::uint64_t(next - previous) * 100), std::chrono::microseconds(5)};
                previous = next;
            }
            return {};
        }
      private:
        motion::Profile m_profile;
        FixedCrossing m_program;
        unsigned m_count{};
        std::uint32_t m_first{}, m_lastPeriod{};
        std::chrono::nanoseconds m_maximum{};
    };
}
