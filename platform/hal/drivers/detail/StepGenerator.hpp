#pragma once

#include "hal/drivers/detail/StepHardware.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"
#include "hal/drivers/itf/IStepGenerator.hpp"
#if defined(HAL_STEP_THREADX)
#include "hal/linux/ThreadMutex.hpp"
#elif !defined(HAL_PLATFORM_STM32)
#include "hal/linux/Mutex.hpp"
#endif

#include <memory>
#include <mutex>
#include <vector>

namespace hal::detail
{
    class StepGenerator final
      : public IStepGenerator
      , public std::enable_shared_from_this<StepGenerator>
    {
      public:
        explicit StepGenerator(std::unique_ptr<StepHardware> hardware,
                               std::array<std::shared_ptr<IDigitalOutput>, 3> pins = {});
        ~StepGenerator() override;
        auto output(step::Axis axis) -> std::shared_ptr<IStepOutput> override;
        auto start() noexcept -> util::Result<> override;
        auto stop() noexcept -> step::Status override;
        auto status() noexcept -> step::Status override;
        auto setProgressCallback(ProgressCallback callback) -> util::Result<> override;
        // Called by DMA/TIM7 IRQs and API operations; Linux uses virtual IRQs.
        auto service() noexcept -> void;
        auto prepare(std::size_t axis,
                     std::span<const step::Timing> sequence,
                     std::optional<step::PulseCount> count) -> util::Result<>;
        auto clear(std::size_t axis) noexcept -> util::Result<>;
        auto startAxis(std::size_t axis, std::chrono::nanoseconds delay) noexcept -> util::Result<>;
        auto stopAxis(std::size_t axis) noexcept -> step::AxisStatus;
        auto setCompletionCallback(std::size_t axis,
                                   const void* owner,
                                   IStepOutput::CompletionCallback callback) -> util::Result<>;
        auto setAxisProgressCallback(std::size_t axis,
                                     const void* owner,
                                     IStepOutput::ProgressCallback callback) -> util::Result<>;
        auto updateTiming(std::size_t axis, step::Timing timing) noexcept -> util::Result<step::PulseCount>;
        auto prepareSequence(std::size_t axis, std::shared_ptr<const step::Sequence> sequence)
          -> util::Result<>;
        auto scheduleCursor(std::size_t axis) noexcept -> util::Result<step::ScheduleCursor>;
        auto replaceSequence(std::size_t axis,
                             step::ScheduleCursor cursor,
                             std::shared_ptr<const step::Sequence> sequence) -> util::Result<>;

      private:
        struct Timing
        {
            std::uint32_t period;
            std::uint32_t high;
        };
        struct Axis
        {
            std::vector<Timing> timings;
            std::shared_ptr<const step::Sequence> sequence;
            std::array<std::array<std::uint32_t, step_buffer_edges / 2U>, 2> periods{};
            std::optional<step::PulseCount> requested;
            std::uint64_t generated{};
            std::uint64_t revision{};
            std::uint64_t completed_pulses{};
            std::uint32_t next_rise{};
            std::uint32_t last_fall{};
            std::uint32_t entries{};
            std::uint8_t target{};
            bool terminal{};
            bool finished{ true };
        };
        struct Observed
        {
            std::uint64_t pulses{};
            bool high{};
            unsigned blocks{};
        };
        auto fill(std::size_t axis, unsigned buffer) noexcept -> bool;
        auto observe(std::size_t axis, const StepSample& sample) const noexcept -> Observed;
        auto updateCounts(const StepSample& sample) noexcept -> void;
        auto deadline(std::uint32_t now) const noexcept -> std::uint32_t;
        auto guard(std::uint32_t now) noexcept -> void;
        auto finish(step::State reason, bool notify) noexcept -> void;
        auto notify() noexcept -> void;
        auto axisStatus(std::size_t axis) const noexcept -> step::AxisStatus;
        static auto convertTiming(step::Timing timing) noexcept -> util::Result<Timing>;
        // Outlives the hardware, which must disconnect AF before pins release.
        std::array<std::shared_ptr<IDigitalOutput>, 3> m_pins;
        std::unique_ptr<StepHardware> m_hardware;
        std::array<Axis, 3> m_axes;
        std::array<step::Timing, step_buffer_edges / 2U> m_sequenceTimings;
        step::Status m_status;
        step::Status m_notified;
        ProgressCallback m_callback;
        struct CompletionListener
        {
            const void* owner{};
            IStepOutput::CompletionCallback callback;
            bool notified{ true };
        };
        std::array<CompletionListener, 3> m_listeners;
        struct ProgressListener
        {
            const void* owner{};
            IStepOutput::ProgressCallback callback;
            step::AxisStatus notified;
            bool active{};
        };
        std::array<ProgressListener, 3> m_progressListeners;
        bool m_inCallback{};
#if defined(HAL_STEP_THREADX)
        mutable linux::ThreadMutex m_mutex;
#elif !defined(HAL_PLATFORM_STM32)
        mutable linux::Mutex m_mutex{ true };
#endif
    };
}
