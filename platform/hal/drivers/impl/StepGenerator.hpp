#pragma once
#include <source_location>

#include "hal/drivers/itf/IDigitalOutput.hpp"
#include "hal/drivers/itf/IStepGenerator.hpp"
#include "hal/drivers/util/StepHardware.hpp"
#if defined(HAL_STEP_THREADX)
#include "hal/linux/ThreadMutex.hpp"
#elif !defined(HAL_PLATFORM_STM32)
#include "hal/linux/Mutex.hpp"
#endif

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace hal::util
{
    class StepGenerator final
      : public IStepGenerator
      , public std::enable_shared_from_this<StepGenerator>
    {
      public:
        StepGenerator(const StepGenerator&) = delete;
        auto operator=(const StepGenerator&) -> StepGenerator& = delete;
        StepGenerator(StepGenerator&&) = delete;
        auto operator=(StepGenerator&&) -> StepGenerator& = delete;

        explicit StepGenerator(std::unique_ptr<StepHardware> hardware,
                               std::array<std::shared_ptr<IDigitalOutput>, 3> pins = {});
        ~StepGenerator() override;
        auto output(step::Axis axis) -> std::shared_ptr<IStepOutput> override;
        auto start() noexcept -> util::Result<> override;
        auto startPrepared(const std::array<std::optional<std::chrono::nanoseconds>, 3>& delays) noexcept
          -> util::Result<> override;
        auto stop() noexcept -> step::Status override;
        auto status() noexcept -> step::Status override;
        auto setProgressCallback(ProgressCallback callback) -> util::Result<> override;
        util::Result<Timeline> timeline() noexcept override;
        util::Result<> setRefillNotification(RefillNotification callback) override;
        void preparePending() noexcept override;
        util::Result<> splice(const std::array<std::shared_ptr<const step::TimedSequence>, 3>& sequences,
                              std::uint32_t preserve_until) override;
        // Called by DMA/TIM7 IRQs and API operations; Linux uses virtual IRQs.
        auto service() noexcept -> void;
        auto prepare(std::size_t index,
                     std::span<const step::Timing> sequence,
                     std::optional<step::PulseCount> count) -> util::Result<>;
        auto clear(std::size_t index) noexcept -> util::Result<>;
        auto startAxis(std::size_t index, std::chrono::nanoseconds delay) noexcept -> util::Result<>;
        auto stopAxis(std::size_t index) noexcept -> step::AxisStatus;
        auto setCompletionCallback(std::size_t index,
                                   const void* owner,
                                   IStepOutput::CompletionCallback callback) -> util::Result<>;
        auto setAxisProgressCallback(std::size_t index,
                                     const void* owner,
                                     IStepOutput::ProgressCallback callback) -> util::Result<>;
        auto updateTiming(std::size_t index, step::Timing timing) noexcept -> util::Result<step::PulseCount>;
        auto prepareSequence(std::size_t index, std::shared_ptr<const step::Sequence> sequence)
          -> util::Result<>;
        auto scheduleCursor(std::size_t index) noexcept -> util::Result<step::ScheduleCursor>;
        auto replaceSequence(std::size_t index,
                             step::ScheduleCursor cursor,
                             std::shared_ptr<const step::Sequence> sequence) -> util::Result<>;

      private:
        struct SpliceWorkspace;
        struct RefillWorkspace;
        struct AxisStart;
        auto captureAxisStart(std::size_t index, AxisStart& start) noexcept -> util::Result<>;
        auto writeStartBatch(std::size_t index,
                             AxisStart& start,
                             std::size_t half,
                             std::uint32_t& entry,
                             std::span<const step::Timing> timings) noexcept -> util::Result<>;
        auto prepareAxisStart(std::size_t index, AxisStart& start) noexcept -> util::Result<>;
        auto publishAxisStart(std::size_t index,
                              const AxisStart& start,
                              std::chrono::nanoseconds delay) noexcept -> util::Result<>;
        struct PreparedStart;
        struct RefillState;
        bool captureRefill(std::size_t i, RefillState& state) noexcept;
        bool calculateRefill(RefillState& state) noexcept;
        bool commitRefill(std::size_t i, const RefillState& state, bool valid) noexcept;
        auto capturePrepared(const std::array<std::optional<std::chrono::nanoseconds>, 3>& delays,
                             PreparedStart& start) noexcept -> util::Result<>;
        auto buildStartHalf(std::size_t i, const PreparedStart& start, unsigned half) noexcept
          -> util::Result<>;
        auto prepareStartAxis(std::size_t i,
                              const PreparedStart& start,
                              std::chrono::nanoseconds delay) noexcept -> util::Result<>;
        auto publishPrepared(const std::array<std::optional<std::chrono::nanoseconds>, 3>& delays,
                             const PreparedStart& start) noexcept -> util::Result<>;
        std::unique_ptr<SpliceWorkspace> m_splice;
        std::unique_ptr<RefillWorkspace> m_refill;
        std::atomic_flag m_splicing{};
        // Initial calculations may run concurrently on distinct inactive channels.
        // A channel remains unarmed until its revision is revalidated.
        std::atomic_uint8_t m_starting{};
        // Caller owns m_splicing; scratch is never referenced by DMA.
        auto reserveScratch(std::uint32_t entries) noexcept -> util::Result<>;
        struct Timing
        {
            std::uint32_t period;
            std::uint32_t high;
        };
        struct Axis
        {
            std::vector<Timing> timings;
            std::shared_ptr<const step::Sequence> sequence;
            std::array<std::array<std::uint32_t, STEP_BUFFER_EDGES / 2U>, 2> periods{};
            std::optional<step::PulseCount> requested;
            std::uint64_t generated{};
            std::uint64_t revision{};
            std::uint64_t completed_pulses{};
            std::uint32_t next_rise{};
            std::uint32_t first_rise{};
            std::optional<std::uint32_t> end_tick;
            std::uint32_t last_fall{};
            std::uint32_t entries{};
            std::uint8_t target{};
            bool terminal{};
            bool finished{ true };
            bool refill_pending{};
            std::uint32_t refill_tick{}, refill_budget{};
        };
        struct Observed
        {
            std::uint64_t pulses{};
            bool high{};
            unsigned blocks{};
        };
        static auto validateDelays(
          const std::array<std::optional<std::chrono::nanoseconds>, 3>& delays) noexcept -> util::Result<>;
        auto armPrepared(const std::array<std::optional<std::chrono::nanoseconds>, 3>& delays,
                         const StepSample& origin) noexcept -> util::Result<>;
        auto copiedSpliceIntervals(unsigned i, const step::TimedSequence& sequence) const -> std::uint32_t;
        auto captureSplice(const std::array<std::shared_ptr<const step::TimedSequence>, 3>& sequences)
          -> util::Result<>;
        auto prepareSplice(const std::array<std::shared_ptr<const step::TimedSequence>, 3>& sequences)
          -> util::Result<>;
        auto commitSplice(const std::array<std::shared_ptr<const step::TimedSequence>, 3>& sequences,
                          std::uint32_t preserve_until) -> util::Result<>;
        auto captureSpliceAxis(unsigned i, const step::TimedSequence& sequence) -> util::Result<>;
        auto certifySpliceAxis(unsigned i, const step::TimedSequence& sequence, bool unchanged) -> bool;
        auto stageSpliceAxis(unsigned i, const step::TimedSequence& sequence) -> util::Result<>;
        auto validSplicePrefix(unsigned i, const step::TimedSequence& sequence) const -> bool;
        auto validateSpliceAxis(unsigned i,
                                const step::TimedSequence& sequence,
                                const StepSample& sample,
                                std::uint32_t preserve_until,
                                std::uint32_t& compare,
                                bool& restart) -> util::Result<>;
        auto publishSpliceAxis(unsigned i,
                               const std::shared_ptr<const step::TimedSequence>& sequence,
                               const StepSample& sample,
                               std::uint32_t compare,
                               bool restart) -> util::Result<>;
        auto fill(std::size_t index, unsigned buffer) noexcept -> bool;
        auto observe(std::size_t index, const StepSample& sample) const noexcept -> Observed;
        bool completeAxisIfDue(std::size_t i, const StepSample& sample, const Observed& observed) noexcept;
        auto updateCounts(const StepSample& sample) noexcept -> void;
        auto deadline(std::uint32_t now) const noexcept -> std::uint32_t;
        auto guard(std::uint32_t now) noexcept -> void;
        auto finish(step::State reason,
                    bool send_notification,
                    unsigned site = std::source_location::current().line()) noexcept -> void;
        auto notify() noexcept -> void;
        auto axisStatus(std::size_t index) const noexcept -> step::AxisStatus;
        static auto convertTiming(step::Timing timing) noexcept -> util::Result<Timing>;
        // Outlives the hardware, which must disconnect AF before pins release.
        std::array<std::shared_ptr<IDigitalOutput>, 3> m_pins;
        std::unique_ptr<StepHardware> m_hardware;
        std::array<Axis, 3> m_axes;
        std::array<step::Timing, STEP_BUFFER_EDGES / 2U> m_sequenceTimings;
        step::Status m_status;
        step::Status m_notified;
        ProgressCallback m_callback;
        RefillNotification m_refillNotification;
        std::atomic_flag m_preparing{};
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
