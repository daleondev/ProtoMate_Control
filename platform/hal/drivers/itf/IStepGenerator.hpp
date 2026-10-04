#pragma once

#include "hal/utilities/Result.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace hal
{
    namespace step
    {
        enum class Axis : std::uint8_t
        {
            _1,
            _2,
            _3
        };
        using PulseCount = std::uint64_t;
        struct Timing
        {
            std::chrono::nanoseconds period{};
            std::chrono::nanoseconds high_time{};
            constexpr bool operator==(const Timing&) const = default;
        };
        enum class State : std::uint8_t
        {
            Idle,
            Ready,
            Running,
            Completed,
            Stopped,
            Underrun,
            DmaError
        };
        struct Status
        {
            // Generator lifecycle: Running includes an idle, ticking timebase.
            State state{ State::Idle };
            std::array<PulseCount, 3> pulses{};
            std::array<State, 3> axes{};
            // False after a DMA error: the last edge may not be reconstructible.
            bool counts_exact{ true };
            constexpr bool operator==(const Status&) const = default;
        };
        struct AxisStatus
        {
            State state{ State::Idle };
            PulseCount pulses{};
            bool counts_exact{ true };
            constexpr bool operator==(const AxisStatus&) const = default;
        };
    }

    // Axis view of a shared generator. Preparing never starts an output.
    class IStepOutput
    {
      public:
        using CompletionCallback = std::move_only_function<void(const step::AxisStatus&) noexcept>;
        virtual ~IStepOutput() = default;
        // Once per started motion on completion, stop or fault. No progress/start
        // notification. One owning view per axis; another view cannot replace/clear it.
        // Register while this axis is stopped; clear at any time outside callbacks.
        // Clearing/view destruction synchronizes with callback execution. ISR rules
        // match the generator callback. Capture must outlive registration.
        [[nodiscard]] virtual auto setCompletionCallback(CompletionCallback callback) -> util::Result<> = 0;
        // Only this axis must be stopped. nullopt is an unlimited counted train.
        // Nonzero count, high/low >= 5 us; timing rounds up to 100 ns ticks.
        [[nodiscard]] virtual auto prepare(step::Timing timing,
                                           std::optional<step::PulseCount> count = std::nullopt)
          -> util::Result<> = 0;
        // One timing per pulse, allowing acceleration. The period starts at
        // that pulse's rising edge. The sequence is copied before returning.
        [[nodiscard]] virtual auto prepareSequence(std::span<const step::Timing> sequence)
          -> util::Result<> = 0;
        // Start this prepared axis on the running timebase; never resets CNT
        // or another axis. Delay >= 5 us, default 1 ms. Very short delays may
        // be rejected with timed_out if setup consumes the scheduling margin.
        [[nodiscard]] virtual auto start(std::chrono::nanoseconds delay = std::chrono::milliseconds{
                                           1 }) noexcept -> util::Result<> = 0;
        // Abort only this axis, force STEP low and retain its pulse count.
        // May truncate the last high phase; this is not a deceleration ramp.
        virtual auto stop() noexcept -> step::AxisStatus = 0;
        [[nodiscard]] virtual auto status() noexcept -> step::AxisStatus = 0;
        // Change the timing of not-yet-buffered pulses without restarting.
        // Returns the first affected pulse number (1-based). Already committed
        // edges remain untouched. Finite count is unchanged. Uniform trains
        // only; prepareSequence supplies exact per-pulse profiles instead.
        // Rejects if every pulse is already buffered, or the new period exceeds
        // the current two-buffer wrap horizon (see README).
        [[nodiscard]] virtual auto updateTiming(step::Timing timing) noexcept
          -> util::Result<step::PulseCount> = 0;
        // Discard this axis's prepared motion. Only this axis must be stopped.
        [[nodiscard]] virtual auto clear() noexcept -> util::Result<> = 0;
        // Commanded rising edges, including an interrupted high phase. This is
        // not measured rotor position. An error means the count is uncertain.
        [[nodiscard]] virtual auto pulseCount() noexcept -> util::Result<step::PulseCount> = 0;
    };

    class IStepGenerator
    {
      public:
        using ProgressCallback = std::move_only_function<void(const step::Status&) noexcept>;
        virtual ~IStepGenerator() = default;
        // Views retain the generator; repeated requests return equivalent views.
        [[nodiscard]] virtual auto output(step::Axis axis) -> std::shared_ptr<IStepOutput> = 0;
        // Start the shared timebase once. Emits no STEP pulses, even when axes
        // have been prepared. Idempotent while running. Axes start explicitly.
        // A fault is latched until stop() followed by start(); no auto-recovery.
        [[nodiscard]] virtual auto start() noexcept -> util::Result<> = 0;
        // Shut down the timebase and every axis immediately; drive STEP low.
        // Can shorten a pulse, which a motor driver may reject.
        virtual auto stop() noexcept -> step::Status = 0;
        [[nodiscard]] virtual auto status() noexcept -> step::Status = 0;
        // Batched progress/completion, NOT one callback per physical edge.
        // STM32: DMA/TIM7 ISR or any caller servicing the generator.
        // Do not block, allocate, mutate or destroy the generator in a callback.
        // Read-only status is permitted.
        // Registration is stopped-only; captures must outlive the generator.
        [[nodiscard]] virtual auto setProgressCallback(ProgressCallback callback) -> util::Result<> = 0;
    };
}
