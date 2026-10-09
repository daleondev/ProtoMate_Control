#pragma once

#include "hal/util/Result.hpp"
#include "hal/drivers/itf/GpioTypes.hpp"
#include "hal/drivers/itf/ITimer.hpp"

#include <chrono>
#include <cstdint>
#include <functional>

namespace hal
{
    class IQuadratureEncoder
    {
      public:
        using Count = std::int64_t;
        struct Sample
        {
            util::Result<Count> position;
            std::chrono::nanoseconds timestamp; // Monotonic, sampled with the count.
            bool running;
        };
        using SampleCallback = std::move_only_function<void(const Sample&) noexcept>;
        static constexpr auto sample_period{ std::chrono::milliseconds{ 10 } };

        virtual ~IQuadratureEncoder() = default;
        IQuadratureEncoder(const IQuadratureEncoder&) = delete;
        IQuadratureEncoder& operator=(const IQuadratureEncoder&) = delete;

        // x4 quadrature counts, not revolutions or commanded motor steps.
        // Start/stop preserve position; construction is stopped at zero.
        [[nodiscard]] virtual auto start() noexcept -> util::Result<> = 0;
        [[nodiscard]] virtual auto stop() noexcept -> util::Result<> = 0;
        [[nodiscard]] virtual auto isRunning() const noexcept -> bool = 0;
        // Requires the backend's interrupt-service deadline to be met. Errors
        // from ambiguous samples or signed overflow remain latched until reset.
        [[nodiscard]] virtual auto position() const noexcept -> util::Result<Count> = 0;
        // Stopped-only origin change; clears a latched count-extension error.
        [[nodiscard]] virtual auto setPosition(Count count) noexcept -> util::Result<> = 0;
        [[nodiscard]] auto reset() noexcept -> util::Result<> { return setPosition(0); }

        // One subscriber; registration/start/stop publish immediately, then
        // every 10 ms while running, including at rest. Delays coalesce; use
        // timestamp differences for speed. Periodic STM32 callbacks run in
        // interrupt context. Keep all callbacks short, nonblocking,
        // and do not call encoder methods from them. Clearing waits for any
        // in-flight callback; configure/clear from thread context only.
        virtual auto setSampleCallback(SampleCallback callback) -> void = 0;
        auto clearSampleCallback() -> void { setSampleCallback({}); }

      protected:
        IQuadratureEncoder() = default;
    };
}

namespace hal::encoder
{
    struct Configuration
    {
        timer::Peripheral timer;
        gpio::Pin a;
        gpio::Pin b;
    };
}
