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
            M1,
            M2,
            M3
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
            State state{ State::Idle };
            std::array<PulseCount, 3> pulses{};
            // False after a DMA error: the last edge may not be reconstructible.
            bool counts_exact{ true };
            constexpr bool operator==(const Status&) const = default;
        };
    }

    // Axis view of a shared generator. Preparing never starts an output.
    class IStepOutput
    {
      public:
        virtual ~IStepOutput() = default;
        // All axes must be stopped. nullopt is an unlimited counted train.
        // Nonzero count, high/low >= 5 us; timing rounds up to 100 ns ticks.
        [[nodiscard]] virtual auto prepare(step::Timing timing,
                                           std::optional<step::PulseCount> count = std::nullopt)
          -> util::Result<> = 0;
        // One timing per pulse, allowing acceleration. The period starts at
        // that pulse's rising edge. The sequence is copied before returning.
        [[nodiscard]] virtual auto prepareSequence(std::span<const step::Timing> sequence)
          -> util::Result<> = 0;
        // Removes this axis from the next group start. Stopped-only.
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
        // Starts every prepared axis against the same counter epoch. Rejected
        // while running; otherwise an explicit restart resets the run counts.
        // Delay applies to all first rising edges and must be >= 5 us.
        [[nodiscard]] virtual auto start(std::chrono::nanoseconds delay = std::chrono::milliseconds{
                                           1 }) noexcept -> util::Result<> = 0;
        // Stops the entire coordinated group immediately and drives STEP low.
        // Can shorten a pulse, which a motor driver may reject.
        virtual auto stop() noexcept -> step::Status = 0;
        [[nodiscard]] virtual auto status() noexcept -> step::Status = 0;
        // Batched progress/completion, NOT one callback per physical edge.
        // STM32: DMA ISR or stop() caller. Do not block, allocate, mutate, or
        // destroy the generator in a callback. Read-only status is permitted.
        // Registration is stopped-only; captures must outlive the generator.
        [[nodiscard]] virtual auto setProgressCallback(ProgressCallback callback) -> util::Result<> = 0;
    };
}
