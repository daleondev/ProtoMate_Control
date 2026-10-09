#pragma once

#include "hal/util/Result.hpp"
#include "hal/drivers/itf/GpioTypes.hpp"
#include "hal/drivers/itf/ITimer.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace hal
{
    class IPwmOutput
    {
      public:
        using PulseCount = std::uint64_t;
        using PulseCallback = std::move_only_function<void(PulseCount) noexcept>;

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
        // Requires configuration. Idempotent in continuous mode; busy during a
        // counted train. The first high phase begins at
        // a timer boundary following an initial inactive arming interval.
        [[nodiscard]] virtual auto start() noexcept -> util::Result<> = 0;
        // Counted train: nullopt runs until stopped, otherwise emit exactly count
        // complete pulses and finish low. Zero count and 0/100% duty are invalid.
        // Starting any counted train while running is rejected (never restarts it).
        // STM32 uses hardware one-pulse mode with interrupt rearming. High time
        // is hardware timed; period is a MINIMUM, extended by interrupt latency.
        [[nodiscard]] virtual auto startPulses(std::optional<PulseCount> count = std::nullopt) noexcept
          -> util::Result<> = 0;
        // Rising edges in the latest counted run, including an in-flight pulse.
        // Reset by a successful start; retained after stop/completion/configure.
        // Ordinary start() is uncounted and resets this to zero.
        [[nodiscard]] virtual auto pulseCount() const noexcept -> PulseCount = 0;
        // Stopped-only registration. Called once per counted rising edge, with
        // the cumulative count. STM32: interrupt context, or stop()'s caller when
        // draining an unserviced edge. Never block, allocate, or change/destroy
        // this output inside the callback; read-only queries are permitted.
        // Referenced objects must outlive the callback. No callbacks in start().
        [[nodiscard]] virtual auto setPulseCallback(PulseCallback callback) noexcept -> util::Result<> = 0;
        // Immediate idle-low stop; can shorten the final pulse. Its rising edge
        // still counts, but a driver may reject a shortened pulse. Destruction
        // stops silently. A pulse count is commanded motion, not encoder feedback.
        [[nodiscard]] virtual auto stop() noexcept -> util::Result<> = 0;
        [[nodiscard]] virtual auto isRunning() const noexcept -> bool = 0;

      protected:
        IPwmOutput() = default;
    };
}

namespace hal::pwm
{
    struct Configuration
    {
        timer::Peripheral timer;
        timer::Channel channel;
        gpio::Pin pin;
    };
}
