#pragma once

#include "hal/drivers/itf/IStepGenerator.hpp"

namespace hal::detail
{
    class StepGenerator;

    class StepOutput final : public IStepOutput
    {
      public:
        StepOutput(std::shared_ptr<StepGenerator> generator, std::size_t axis);
        ~StepOutput() override;
        StepOutput(const StepOutput&) = delete;
        StepOutput& operator=(const StepOutput&) = delete;
        StepOutput(StepOutput&&) = delete;
        StepOutput& operator=(StepOutput&&) = delete;

        auto setProgressCallback(ProgressCallback callback) -> util::Result<> override;
        auto setCompletionCallback(CompletionCallback callback) -> util::Result<> override;
        auto prepare(step::Timing timing, std::optional<step::PulseCount> count) -> util::Result<> override;
        auto prepareSequence(std::span<const step::Timing> sequence) -> util::Result<> override;
        auto prepareSequence(std::shared_ptr<const step::Sequence> sequence) -> util::Result<> override;
        auto scheduleCursor() noexcept -> util::Result<step::ScheduleCursor> override;
        auto replaceSequence(step::ScheduleCursor cursor, std::shared_ptr<const step::Sequence> sequence)
          -> util::Result<> override;
        auto start(std::chrono::nanoseconds delay) noexcept -> util::Result<> override;
        auto stop() noexcept -> step::AxisStatus override;
        auto status() noexcept -> step::AxisStatus override;
        auto updateTiming(step::Timing timing) noexcept -> util::Result<step::PulseCount> override;
        auto clear() noexcept -> util::Result<> override;
        auto pulseCount() noexcept -> util::Result<step::PulseCount> override;

      private:
        std::shared_ptr<StepGenerator> m_generator;
        std::size_t m_axis;
    };
}
