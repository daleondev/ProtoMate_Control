#include "hal/drivers/impl/StepOutput.hpp"
#include "hal/drivers/impl/StepGenerator.hpp"
#include <utility>

namespace hal::util
{
    StepOutput::StepOutput(std::shared_ptr<StepGenerator> generator, std::size_t axis)
      : m_generator{ std::move(generator) }
      , m_axis{ axis }
    {
    }
    StepOutput::~StepOutput()
    {
        static_cast<void>(m_generator->setAxisProgressCallback(m_axis, this, {}));
        static_cast<void>(m_generator->setCompletionCallback(m_axis, this, {}));
    }
    auto StepOutput::setProgressCallback(ProgressCallback callback) -> util::Result<>
    {
        return m_generator->setAxisProgressCallback(m_axis, this, std::move(callback));
    }
    auto StepOutput::setCompletionCallback(CompletionCallback callback) -> util::Result<>
    {
        return m_generator->setCompletionCallback(m_axis, this, std::move(callback));
    }
    auto StepOutput::prepare(step::Timing timing, std::optional<step::PulseCount> count) -> util::Result<>
    {
        return m_generator->prepare(m_axis, std::span{ &timing, 1U }, count);
    }
    auto StepOutput::prepareSequence(std::span<const step::Timing> sequence) -> util::Result<>
    {
        return m_generator->prepare(m_axis, sequence, sequence.size());
    }
    auto StepOutput::prepareSequence(std::shared_ptr<const step::Sequence> sequence) -> util::Result<>
    {
        return m_generator->prepareSequence(m_axis, std::move(sequence));
    }
    auto StepOutput::scheduleCursor() noexcept -> util::Result<step::ScheduleCursor>
    {
        return m_generator->scheduleCursor(m_axis);
    }
    auto StepOutput::replaceSequence(step::ScheduleCursor cursor,
                                     std::shared_ptr<const step::Sequence> sequence) -> util::Result<>
    {
        return m_generator->replaceSequence(m_axis, cursor, std::move(sequence));
    }
    auto StepOutput::start(std::chrono::nanoseconds delay) noexcept -> util::Result<>
    {
        return m_generator->startAxis(m_axis, delay);
    }
    auto StepOutput::stop() noexcept -> step::AxisStatus { return m_generator->stopAxis(m_axis); }
    auto StepOutput::status() noexcept -> step::AxisStatus
    {
        const auto group{ m_generator->status() };
        return { group.axes[m_axis], group.pulses[m_axis], group.counts_exact, group.periods[m_axis] };
    }
    auto StepOutput::updateTiming(step::Timing timing) noexcept -> util::Result<step::PulseCount>
    {
        return m_generator->updateTiming(m_axis, timing);
    }
    auto StepOutput::clear() noexcept -> util::Result<> { return m_generator->clear(m_axis); }
    auto StepOutput::pulseCount() noexcept -> util::Result<step::PulseCount>
    {
        const auto value{ m_generator->status() };
        if (!value.counts_exact) {
            return std::unexpected(std::make_error_code(std::errc::io_error));
        }
        return value.pulses[m_axis];
    }
}
