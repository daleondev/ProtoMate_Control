#pragma once

#include "hal/utilities/Result.hpp"

#include <chrono>

namespace hal
{
    class IPwmOutput
    {
      public:
        struct Timing
        {
            std::chrono::nanoseconds period{};
            std::chrono::nanoseconds high_time{};
            constexpr bool operator==(const Timing&) const = default;
        };

        virtual ~IPwmOutput() = default;
        IPwmOutput(const IPwmOutput&) = delete;
        IPwmOutput& operator=(const IPwmOutput&) = delete;

        // Stopped-only configuration. Invalid requests leave the old timing intact.
        // Supports 0..100% duty. Interior duty requests must retain both pulse phases.
        [[nodiscard]] virtual auto configure(Timing timing) noexcept -> util::Result<> = 0;
        // Actual hardware timing, rounded up to whole nanoseconds; zero until configured.
        [[nodiscard]] virtual auto timing() const noexcept -> Timing = 0;
        // Requires configuration. Idempotent; the first high phase begins at
        // a timer boundary following an initial inactive arming interval.
        [[nodiscard]] virtual auto start() noexcept -> util::Result<> = 0;
        // Immediate idle-low stop; can shorten the final pulse. Not a pulse-count API.
        [[nodiscard]] virtual auto stop() noexcept -> util::Result<> = 0;
        [[nodiscard]] virtual auto isRunning() const noexcept -> bool = 0;

      protected:
        IPwmOutput() = default;
    };
}
