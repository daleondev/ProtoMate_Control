#pragma once

#include "hal/drivers/util/StepHardware.hpp"

#include <cstdint>
#include <optional>

namespace clock_reference
{
    constexpr std::uint32_t nominal_hz{ 10'000'000U };
    constexpr std::uint32_t window_seconds{ 10U };
    constexpr std::uint32_t max_poll_ticks{ 50'000U }; // Nominal 5 ms.
    constexpr double screen_ppm{ 1000.0 };             // Diagnostic 0.1%, not a robot specification.
    // Allow 100 us per boundary for RTC shadow synchronization (2 LSE cycles
    // are about 61 us). This is separate from the measured polling bracket.
    constexpr std::uint32_t sync_margin_ticks{ 1000U };

    struct Sample
    {
        unsigned second{};
        std::uint32_t before{}, after{};
    };
    struct Boundary
    {
        std::uint32_t before{}, after{};
    };
    enum class Observation
    {
        Waiting,
        Boundary,
        Invalid
    };

    class Tracker
    {
      public:
        auto observe(Sample sample) noexcept -> Observation
        {
            using hal::util::stepDistance;
            if (sample.second >= 60U || sample.before >= hal::util::step_park ||
                sample.after >= hal::util::step_park ||
                stepDistance(sample.before, sample.after) > max_poll_ticks) {
                return Observation::Invalid;
            }
            if (!m_previous) {
                m_previous = sample;
                return Observation::Waiting;
            }
            const auto previous{ *m_previous };
            m_previous = sample;
            if (stepDistance(previous.before, sample.after) > max_poll_ticks) {
                return Observation::Invalid;
            }
            if (sample.second == previous.second) {
                return Observation::Waiting;
            }
            if (sample.second != (previous.second + 1U) % 60U) {
                return Observation::Invalid;
            }
            m_boundary = { previous.before, sample.after };
            return Observation::Boundary;
        }
        auto boundary() const noexcept -> Boundary { return m_boundary; }

      private:
        std::optional<Sample> m_previous;
        Boundary m_boundary;
    };

    struct Measurement
    {
        double ticks{}, hz{}, error_ppm{}, sampling_bound_ppm{};
        enum class Screen
        {
            Within,
            Fast,
            Slow,
            Inconclusive
        } screen{};
    };

    // Both endpoints enclose an observed RTC second transition. Timer wrapping
    // uses ARR+1 = 0xFFFFFFFF, not the usual uint32_t modulus of 0x100000000.
    inline auto measure(Boundary first, Boundary last) noexcept -> Measurement
    {
        using hal::util::stepDistance;
        const double lower{ static_cast<double>(stepDistance(first.after, last.before)) -
                            2.0 * sync_margin_ticks };
        const double upper{ static_cast<double>(stepDistance(first.before, last.after)) +
                            2.0 * sync_margin_ticks };
        const double ticks{ (lower + upper) / 2.0 };
        const double expected{ static_cast<double>(nominal_hz) * window_seconds };
        const double ppm{ (ticks - expected) * 1e6 / expected };
        const double bound{ (upper - lower) * .5e6 / expected };
        auto screen{ Measurement::Screen::Inconclusive };
        if (ppm - bound > screen_ppm) {
            screen = Measurement::Screen::Fast;
        }
        else if (ppm + bound < -screen_ppm) {
            screen = Measurement::Screen::Slow;
        }
        else if (ppm - bound >= -screen_ppm && ppm + bound <= screen_ppm) {
            screen = Measurement::Screen::Within;
        }
        return { ticks, ticks / window_seconds, ppm, bound, screen };
    }

    inline auto screenName(Measurement::Screen screen) noexcept -> const char*
    {
        switch (screen) {
            case Measurement::Screen::Within:
                return "WITHIN_0.1_PERCENT";
            case Measurement::Screen::Fast:
                return "FAST";
            case Measurement::Screen::Slow:
                return "SLOW";
            case Measurement::Screen::Inconclusive:
                return "INCONCLUSIVE";
        }
        return "INCONCLUSIVE";
    }
}
