#include "hal/drivers/detail/SimulatedStepHardware.hpp"
#include "hal/drivers/detail/StepGenerator.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>

namespace
{
    using namespace std::chrono_literals;
    using hal::step::Axis;
    using hal::step::State;
    class StepTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            auto port{ std::make_unique<hal::detail::SimulatedStepHardware>() };
            hardware = port.get();
            generator = std::make_shared<hal::detail::StepGenerator>(std::move(port));
            hardware->interrupt = [this] { generator->service(); };
            for (unsigned i = 0; i < 3U; ++i) {
                outputs[i] = generator->output(static_cast<Axis>(i));
            }
        }
        auto startMoves(std::chrono::nanoseconds delay) -> bool
        {
            static_cast<void>(generator->stop());
            if (!generator->start())
                return false;
            for (const auto& axis : outputs) {
                if (axis->status().state == State::Ready && !axis->start(delay))
                    return false;
            }
            return true;
        }
        auto rises(unsigned axis) const -> std::vector<std::uint64_t>
        {
            std::vector<std::uint64_t> result;
            for (auto edge : hardware->edges) {
                if (edge.axis == axis && edge.high) {
                    result.push_back(edge.tick);
                }
            }
            return result;
        }
        hal::detail::SimulatedStepHardware* hardware{};
        std::shared_ptr<hal::detail::StepGenerator> generator;
        std::array<std::shared_ptr<hal::IStepOutput>, 3> outputs;
    };
}

TEST_F(StepTest, FiniteMovesFinishLowWithoutAnInterruptOrExtraWrapPulse)
{
    hardware->interrupts_enabled = false;
    ASSERT_TRUE(outputs[0]->prepare({ 100us, 5us }, 3));
    ASSERT_TRUE(startMoves(10us));
    hardware->advance(hal::detail::step_park * 3ULL);
    EXPECT_EQ(rises(0), (std::vector<std::uint64_t>{ 100, 1100, 2100 }));
    EXPECT_FALSE(hardware->high[0]);
    EXPECT_FALSE(hardware->registers.running);
    auto status{ generator->status() };
    EXPECT_EQ(status.state, State::Underrun);
    EXPECT_EQ(status.axes[0], State::Completed);
    EXPECT_EQ(status.pulses[0], 3U);
    EXPECT_TRUE(status.counts_exact);
}

TEST_F(StepTest, ThreeIndependentRatesShareTheFirstEdgeAndKeepExactSpacingAcrossBuffers)
{
    constexpr std::array periods{ 100us, 150us, 210us };
    constexpr std::array counts{ 1541U, 1024U, 700U };
    for (unsigned i = 0; i < 3; ++i) {
        ASSERT_TRUE(outputs[i]->prepare({ periods[i], 5us }, counts[i]));
    }
    ASSERT_TRUE(startMoves(1ms));
    hardware->advance(3'000'000U);
    const auto status{ generator->status() };
    EXPECT_EQ(status.state, State::Running);
    EXPECT_EQ(status.axes[0], State::Completed);
    for (unsigned axis = 0; axis < 3; ++axis) {
        const auto observed{ rises(axis) };
        ASSERT_EQ(observed.size(), counts[axis]);
        EXPECT_EQ(observed.front(), 10'000U);
        for (std::size_t i = 1; i < observed.size(); ++i) {
            EXPECT_EQ(observed[i] - observed[i - 1], static_cast<std::uint64_t>(periods[axis].count() * 10U));
        }
        EXPECT_EQ(status.pulses[axis], counts[axis]);
        EXPECT_FALSE(hardware->high[axis]);
    }
}

TEST_F(StepTest, DelayedBufferInterruptDoesNotStretchPulseIntervals)
{
    ASSERT_TRUE(outputs[0]->prepare({ 10us, 5us }, 2000));
    ASSERT_TRUE(startMoves(10us));
    hardware->interrupts_enabled = false;
    hardware->advance(40'000); // More than a buffer, less than the two-buffer guard.
    generator->service();
    hardware->interrupts_enabled = true;
    hardware->advance(1'000'000);
    const auto observed{ rises(0) };
    ASSERT_EQ(observed.size(), 2000U);
    for (std::size_t i = 1; i < observed.size(); ++i) {
        EXPECT_EQ(observed[i] - observed[i - 1], 100U);
    }
    EXPECT_EQ(generator->status().state, State::Running);
    EXPECT_EQ(outputs[0]->status().state, State::Completed);
}

TEST_F(StepTest, BenchIrqDelayAndUnderrunHaveTheDocumentedThreeAxisWaveforms)
{
    constexpr std::array periods{ 10us, 20us, 40us };
    for (bool underrun : { false, true }) {
        for (unsigned i = 0; i < 3U; ++i) {
            ASSERT_TRUE(
              outputs[i]->prepare({ periods[i], 5us }, underrun ? std::nullopt : std::optional{ 1000U }));
        }
        ASSERT_TRUE(startMoves(1ms));
        hardware->interrupts_enabled = false;
        hardware->advance(underrun ? 200'000U : 45'000U);
        EXPECT_EQ(hardware->registers.running, !underrun);
        EXPECT_TRUE(hardware->registers.channels[0].transfer_complete);
        generator->service();
        hardware->interrupts_enabled = true;
        hardware->advance(1'000'000U);
        const auto status{ generator->status() };
        EXPECT_EQ(status.state, underrun ? State::Underrun : State::Running);
        const std::array expected{ underrun ? 512U : 1000U,
                                   underrun ? 256U : 1000U,
                                   underrun ? 128U : 1000U };
        for (unsigned i = 0; i < 3U; ++i) {
            const auto edges{ rises(i) };
            ASSERT_EQ(edges.size(), expected[i]);
            EXPECT_EQ(status.pulses[i], expected[i]);
            EXPECT_FALSE(hardware->high[i]);
            for (std::size_t edge = 1; edge < edges.size(); ++edge) {
                EXPECT_EQ(edges[edge] - edges[edge - 1], periods[i].count() * 10U);
            }
        }
    }
}

TEST_F(StepTest, MissedRefillStopsHardwareBeforeReplayingAStaleBuffer)
{
    ASSERT_TRUE(outputs[0]->prepare({ 10us, 5us }));
    ASSERT_TRUE(outputs[1]->prepare({ 31us, 9us }));
    ASSERT_TRUE(startMoves(10us));
    hardware->interrupts_enabled = false;
    hardware->advance(hal::detail::step_park * 2ULL);
    EXPECT_FALSE(hardware->registers.running);
    EXPECT_EQ(rises(0).size(), 512U);
    const auto status{ generator->status() };
    EXPECT_EQ(status.state, State::Underrun);
    EXPECT_TRUE(status.counts_exact);
    EXPECT_EQ(status.pulses[0], rises(0).size());
    EXPECT_EQ(status.pulses[1], rises(1).size());
    EXPECT_FALSE(hardware->high[0]);
    EXPECT_FALSE(hardware->high[1]);
    generator->service();
    hardware->advance(1000000U);
    EXPECT_EQ(rises(0).size(), 512U);
}

TEST_F(StepTest, CountsAnEdgeWhoseDmaTransferIsStillPendingAndRetainsItAfterAbort)
{
    ASSERT_TRUE(outputs[0]->prepare({ 100us, 5us }, 1));
    ASSERT_TRUE(startMoves(10us));
    hardware->hold_dma[0] = true;
    hardware->advance(100U);
    EXPECT_EQ(outputs[0]->pulseCount(), 1U);
    EXPECT_EQ(generator->stop().state, State::Stopped);
    EXPECT_EQ(outputs[0]->pulseCount(), 1U);
    EXPECT_FALSE(hardware->high[0]);
}

TEST_F(StepTest, SequenceIsCopiedAndAccelerationCrossesBufferBoundariesWithoutGaps)
{
    std::vector<hal::step::Timing> sequence;
    std::uint64_t expected_time{ 100U };
    std::vector<std::uint64_t> expected;
    for (unsigned i = 0; i < 1000U; ++i) {
        const auto period{ std::chrono::microseconds{ 1000 - i } + 10us };
        sequence.push_back({ period, 5us });
        expected.push_back(expected_time);
        expected_time += period.count() * 10U;
    }
    ASSERT_TRUE(outputs[0]->prepareSequence(sequence));
    sequence.clear();
    ASSERT_TRUE(startMoves(10us));
    hardware->advance(expected_time + 100U);
    EXPECT_EQ(rises(0), expected);
    EXPECT_EQ(generator->status().state, State::Running);
    EXPECT_EQ(outputs[0]->status().state, State::Completed);
}

TEST_F(StepTest, CounterWrapDoesNotChangeSpacingAndParkedAxesStayLow)
{
    ASSERT_TRUE(outputs[0]->prepare({ 17s, 5us }, 70));
    ASSERT_TRUE(outputs[1]->prepare({ 1s, 5us }, 429));
    ASSERT_TRUE(outputs[2]->prepare({ 1s, 5us }, 1));
    ASSERT_TRUE(startMoves(10us));
    hardware->advance(12'000'000'000ULL);
    EXPECT_EQ(generator->status().state, State::Running);
    EXPECT_EQ(outputs[0]->status().state, State::Completed);
    ASSERT_EQ(rises(0).size(), 70U);
    ASSERT_EQ(rises(1).size(), 429U);
    ASSERT_EQ(rises(2).size(), 1U);
    const auto observed{ rises(0) };
    for (std::size_t i = 1; i < observed.size(); ++i) {
        EXPECT_EQ(observed[i] - observed[i - 1], 170'000'000ULL);
    }
}

TEST_F(StepTest, TimebaseAndAxesHaveIndependentLifecycles)
{
    EXPECT_FALSE(outputs[0]->start());
    ASSERT_TRUE(generator->start());
    hardware->advance(hal::detail::step_park * 2ULL);
    EXPECT_TRUE(hardware->registers.running);
    EXPECT_TRUE(hardware->edges.empty());
    EXPECT_EQ(generator->output(static_cast<Axis>(255)), nullptr);
    ASSERT_TRUE(outputs[0]->prepare({ 100us, 5us }, 2));
    EXPECT_FALSE(outputs[0]->prepare({ 1us, 1us }, 5));
    EXPECT_FALSE(outputs[0]->prepare({ 100us, 5us }, 0));
    EXPECT_FALSE(outputs[0]->prepareSequence({}));
    EXPECT_FALSE(outputs[0]->start(0ns));
    ASSERT_TRUE(outputs[0]->start(10us));
    EXPECT_FALSE(outputs[0]->start(10us));
    EXPECT_FALSE(outputs[0]->prepare({ 100us, 5us }, 1));
    ASSERT_TRUE(generator->start()); // Idempotent, does not reset counts/phase.
    ASSERT_TRUE(outputs[1]->prepare({ 100us, 5us }, 1));
    EXPECT_FALSE(outputs[0]->clear());
    hardware->advance(3000U);
    EXPECT_EQ(outputs[0]->status().state, State::Completed);
    EXPECT_EQ(rises(1).size(), 0U); // prepare alone never starts a channel.
    ASSERT_TRUE(outputs[0]->start(10us));
    EXPECT_EQ(outputs[0]->pulseCount(), 0U);
    hardware->advance(3000U);
    EXPECT_EQ(outputs[0]->pulseCount(), 2U);
    ASSERT_TRUE(outputs[0]->clear());
    EXPECT_FALSE(outputs[0]->start());
    EXPECT_TRUE(hardware->registers.running);
}

TEST_F(StepTest, ErrorDoesNotClaimAnExactPositionOrRestartMotion)
{
    ASSERT_TRUE(outputs[0]->prepare({ 100us, 5us }));
    ASSERT_TRUE(startMoves(10us));
    hardware->advance(105U);
    hardware->registers.error = true;
    generator->service();
    EXPECT_EQ(generator->status().state, State::DmaError);
    EXPECT_FALSE(generator->status().counts_exact);
    EXPECT_EQ(outputs[0]->pulseCount().error(), std::errc::io_error);
    EXPECT_FALSE(hardware->registers.running);
    EXPECT_FALSE(hardware->high[0]);
}

TEST_F(StepTest, BatchedCallbacksMayQueryStateButCannotMutateTheGenerator)
{
    unsigned callbacks{};
    ASSERT_TRUE(generator->setProgressCallback([&](const auto& status) noexcept {
        ++callbacks;
        EXPECT_EQ(generator->status().pulses, status.pulses);
        EXPECT_FALSE(outputs[0]->clear());
        EXPECT_FALSE(generator->start());
    }));
    ASSERT_TRUE(outputs[0]->prepare({ 100us, 5us }, 1300));
    ASSERT_TRUE(startMoves(10us));
    hardware->advance(2'000'000U);
    EXPECT_GT(callbacks, 1U);
    EXPECT_LT(callbacks, 130U); // Batched DMA / 1 ms tail observations, not one per pulse.
    EXPECT_EQ(generator->status().state, State::Running);
    EXPECT_EQ(outputs[0]->status().state, State::Completed);
}

TEST_F(StepTest, LargeTargetsUseBoundedBuffersAndContinuousOperationCanBeAborted)
{
    ASSERT_TRUE(outputs[0]->prepare({ 10us, 5us }, std::numeric_limits<std::uint64_t>::max()));
    ASSERT_TRUE(outputs[1]->prepare({ 20us, 5us }));
    ASSERT_TRUE(startMoves(10us));
    hardware->advance(100'000U);
    EXPECT_EQ(generator->status().state, State::Running);
    EXPECT_EQ(generator->stop().state, State::Stopped);
    EXPECT_EQ(generator->status().pulses[0], rises(0).size());
    EXPECT_EQ(generator->status().pulses[1], rises(1).size());
    EXPECT_GT(rises(0).size(), 512U);
    EXPECT_LE(hardware->entries[0], 512U);
}

TEST_F(StepTest, TerminalPulseAtEveryBufferBoundaryFinishesExactly)
{
    for (const unsigned count : { 1U, 127U, 128U, 129U, 255U, 256U, 257U, 511U, 512U, 513U, 768U, 769U }) {
        SCOPED_TRACE(count);
        ASSERT_TRUE(outputs[0]->prepare({ 10us, 5us }, count));
        ASSERT_TRUE(startMoves(10us));
        hardware->advance(200'000U);
        EXPECT_EQ(generator->status().state, State::Running);
        EXPECT_EQ(outputs[0]->status().state, State::Completed);
        EXPECT_EQ(rises(0).size(), count);
        EXPECT_EQ(outputs[0]->pulseCount(), count);
        EXPECT_FALSE(hardware->high[0]);
    }
}

TEST_F(StepTest, PendingTerminalDmaCannotLeaveAnOldCompareThatRepeatsAfterWrap)
{
    ASSERT_TRUE(outputs[0]->prepare({ 100us, 5us }, 1));
    ASSERT_TRUE(outputs[1]->prepare({ 17s, 5us }, 30));
    ASSERT_TRUE(startMoves(10us));
    hardware->advance(100U); // First rise loaded its falling timestamp.
    hardware->hold_dma[0] = true;
    hardware->advance(50U); // Last fall happened, terminal DMA still pending.
    generator->service();
    EXPECT_EQ(outputs[0]->pulseCount(), 1U);
    hardware->advance(6'000'000'000ULL);
    EXPECT_EQ(generator->status().state, State::Running);
    EXPECT_EQ(outputs[0]->status().state, State::Completed);
    EXPECT_EQ(rises(0).size(), 1U);
    EXPECT_EQ(rises(1).size(), 30U);
}

TEST_F(StepTest, AnExpiringGuardDuringRefillCannotBeUndoneByPublishingNewData)
{
    class SlowPublish final : public hal::detail::SimulatedStepHardware
    {
      public:
        bool delay{};
        void publish() noexcept override
        {
            if (delay) {
                interrupts_enabled = false;
                advance(100'000U);
            }
        }
    };
    auto port{ std::make_unique<SlowPublish>() };
    auto* slow{ port.get() };
    auto engine{ std::make_shared<hal::detail::StepGenerator>(std::move(port)) };
    auto axis{ engine->output(Axis::_1) };
    ASSERT_TRUE(axis->prepare({ 10us, 5us }));
    ASSERT_TRUE(engine->start());
    ASSERT_TRUE(axis->start(10us));
    slow->interrupts_enabled = false;
    slow->advance(30'000U);
    slow->delay = true;
    engine->service();
    EXPECT_EQ(engine->status().state, State::Underrun);
    EXPECT_FALSE(slow->registers.running);
    EXPECT_EQ(engine->status().pulses[0], 512U);
}

TEST_F(StepTest, DmaLoadingAMissedTimestampDoesNotInventAnEdgeFromStickyCompareFlags)
{
    ASSERT_TRUE(outputs[0]->prepare({ 100us, 5us }, 10));
    ASSERT_TRUE(startMoves(10us));
    hardware->advance(100U);
    hardware->hold_dma[0] = true;
    hardware->advance(1100U); // Last fall occurred; next rising timestamp was not loaded.
    hardware->hold_dma[0] = false;
    hardware->transfer(0U); // Loads an already missed rising compare.
    EXPECT_EQ(rises(0).size(), 1U);
    EXPECT_EQ(outputs[0]->pulseCount(), 1U);
    hardware->advance(40'000U);
    EXPECT_EQ(generator->status().state, State::Underrun);
    EXPECT_EQ(outputs[0]->pulseCount(), 1U);
}

TEST_F(StepTest, StaggeredStartStopRestartDoesNotChangeAnotherAxisTimeline)
{
    ASSERT_TRUE(generator->start());
    ASSERT_TRUE(outputs[0]->prepare({ 10us, 5us }, 3000));
    ASSERT_TRUE(outputs[0]->start(10us));
    hardware->advance(10'025U);
    ASSERT_TRUE(outputs[1]->prepare({ 20us, 5us }));
    ASSERT_TRUE(outputs[1]->start(10us));
    hardware->advance(10'500U);
    const auto stopped{ outputs[1]->stop() };
    EXPECT_EQ(stopped.state, State::Stopped);
    EXPECT_EQ(stopped.pulses, rises(1).size());
    EXPECT_TRUE(hardware->registers.running);
    EXPECT_EQ(outputs[0]->status().state, State::Running);
    ASSERT_TRUE(outputs[1]->prepare({ 30us, 5us }, 10));
    ASSERT_TRUE(outputs[1]->start(10us));
    hardware->advance(400'000U);
    EXPECT_EQ(outputs[1]->pulseCount(), 10U);
    EXPECT_EQ(outputs[0]->pulseCount(), 3000U);
    const auto edges{ rises(0) };
    ASSERT_EQ(edges.size(), 3000U);
    for (std::size_t i = 1; i < edges.size(); ++i)
        EXPECT_EQ(edges[i] - edges[i - 1], 100U);
    EXPECT_EQ(rises(2).size(), 0U);
    EXPECT_TRUE(hardware->registers.running);
}

TEST_F(StepTest, LiveTimingChangesAtReportedPulseAndPreservesFiniteCount)
{
    ASSERT_TRUE(generator->start());
    ASSERT_TRUE(outputs[0]->prepare({ 20us, 5us }, 1600));
    ASSERT_TRUE(outputs[1]->prepare({ 30us, 5us }, 2000));
    ASSERT_TRUE(outputs[0]->start(10us));
    ASSERT_TRUE(outputs[1]->start(10us));
    hardware->advance(5000U);
    const auto change{ outputs[0]->updateTiming({ 10us, 5us }) };
    ASSERT_TRUE(change);
    EXPECT_EQ(*change, 513U);
    hardware->advance(20'000U);
    const auto replacement{ outputs[0]->updateTiming({ 40us, 10us }) };
    ASSERT_TRUE(replacement);
    EXPECT_EQ(*replacement, *change); // Last update wins until that boundary is filled.
    hardware->advance(1'000'000U);
    const auto edges{ rises(0) };
    ASSERT_EQ(edges.size(), 1600U);
    for (std::size_t i = 1; i < edges.size(); ++i) {
        EXPECT_EQ(edges[i] - edges[i - 1], i >= *change ? 400U : 200U);
    }
    const auto other{ rises(1) };
    ASSERT_EQ(other.size(), 2000U);
    for (std::size_t i = 1; i < other.size(); ++i)
        EXPECT_EQ(other[i] - other[i - 1], 300U);
    EXPECT_EQ(outputs[0]->status().state, State::Completed);
    EXPECT_EQ(outputs[0]->pulseCount(), 1600U);
    EXPECT_TRUE(hardware->registers.running);
}

TEST_F(StepTest, ContinuousTimingUpdateAndFailedUpdatesNeverResetPositionOrPhase)
{
    ASSERT_TRUE(generator->start());
    ASSERT_TRUE(outputs[0]->prepare({ 10us, 5us }));
    ASSERT_TRUE(outputs[0]->start(10us));
    hardware->advance(7000U);
    const auto count{ outputs[0]->pulseCount() };
    EXPECT_FALSE(outputs[0]->updateTiming({ 5us, 5us }));
    EXPECT_FALSE(outputs[0]->updateTiming({ 1s, 5us })); // Exceeds this move's buffer horizon.
    EXPECT_EQ(outputs[0]->pulseCount(), count);
    const auto change{ outputs[0]->updateTiming({ 20us, 5us }) };
    ASSERT_TRUE(change);
    hardware->advance(200'000U);
    const auto final{ outputs[0]->stop() };
    const auto edges{ rises(0) };
    EXPECT_EQ(final.pulses, edges.size());
    for (std::size_t i = 1; i < edges.size(); ++i) {
        EXPECT_EQ(edges[i] - edges[i - 1], i >= *change ? 200U : 100U);
    }
    EXPECT_TRUE(hardware->registers.running);
}

TEST_F(StepTest, IndependentStartAcrossWrapAndShortCompletionDoNotStopTheClock)
{
    ASSERT_TRUE(generator->start());
    hardware->advance(hal::detail::step_park - 75U);
    ASSERT_TRUE(outputs[0]->prepare({ 10us, 5us }, 1));
    ASSERT_TRUE(outputs[0]->start(10us));
    hardware->advance(500'000U);
    EXPECT_EQ(rises(0), (std::vector<std::uint64_t>{ std::uint64_t{ hal::detail::step_park } + 25U }));
    EXPECT_EQ(outputs[0]->status().state, State::Completed);
    EXPECT_FALSE(hardware->completion_watch);
    hardware->advance(hal::detail::step_park * 2ULL);
    EXPECT_EQ(rises(0).size(), 1U);
    EXPECT_TRUE(hardware->registers.running);
}

TEST_F(StepTest, AxisStopIncludesPendingDmaAndDoesNotDisarmTheOtherAxis)
{
    ASSERT_TRUE(generator->start());
    for (auto& axis : outputs)
        ASSERT_TRUE(axis->prepare({ 10us, 5us }));
    for (auto& axis : outputs)
        ASSERT_TRUE(axis->start(10us));
    hardware->hold_dma[0] = true;
    hardware->advance(100U);
    EXPECT_EQ(outputs[0]->stop().pulses, 1U);
    EXPECT_FALSE(hardware->high[0]);
    hardware->advance(50'000U);
    EXPECT_EQ(rises(0).size(), 1U);
    EXPECT_EQ(rises(1).size(), 501U);
    EXPECT_EQ(rises(2).size(), 501U);
    EXPECT_EQ(outputs[1]->status().state, State::Running);
}

TEST_F(StepTest, FaultCannotBeSilentlyRestartedAndOnlyExplicitShutdownAcknowledgesIt)
{
    ASSERT_TRUE(startMoves(10us));
    ASSERT_TRUE(outputs[0]->prepare({ 10us, 5us }));
    ASSERT_TRUE(outputs[0]->start(10us));
    hardware->interrupts_enabled = false;
    hardware->advance(100'000U);
    EXPECT_EQ(generator->status().state, State::Underrun);
    EXPECT_FALSE(generator->start());
    EXPECT_FALSE(outputs[0]->start());
    static_cast<void>(generator->stop());
    ASSERT_TRUE(generator->start());
    EXPECT_EQ(outputs[0]->status().state, State::Ready);
    EXPECT_EQ(rises(0).size(), 0U); // Restarting the service does not restart a move.
}

TEST(StepArming, MissedStartMarginRejectsOnlyTheNewAxisAndPreservesItsPreparedMove)
{
    class DelayedArm final : public hal::detail::SimulatedStepHardware
    {
      public:
        bool delay{};
        auto arm(std::size_t axis, std::uint32_t first, std::uint32_t count) noexcept -> bool override
        {
            if (delay)
                advance(200U); // Setup consumed 20 us of a requested 10 us lead.
            return SimulatedStepHardware::arm(axis, first, count);
        }
    };
    auto backend{ std::make_unique<DelayedArm>() };
    auto* hardware{ backend.get() };
    auto engine{ std::make_shared<hal::detail::StepGenerator>(std::move(backend)) };
    hardware->interrupt = [&] { engine->service(); };
    const auto moving{ engine->output(Axis::_1) };
    const auto joining{ engine->output(Axis::_2) };
    ASSERT_TRUE(engine->start());
    ASSERT_TRUE(moving->prepare({ 20us, 5us }, 100));
    ASSERT_TRUE(moving->start(10us));
    hardware->advance(1000U);
    ASSERT_TRUE(joining->prepare({ 10us, 5us }, 1));
    hardware->delay = true;
    EXPECT_EQ(joining->start(10us).error(), std::errc::timed_out);
    EXPECT_EQ(joining->status().state, State::Ready);
    EXPECT_EQ(joining->pulseCount(), 0U);
    EXPECT_EQ(moving->status().state, State::Running);
    hardware->delay = false;
    ASSERT_TRUE(joining->start(10us));
    hardware->advance(100'000U);
    EXPECT_EQ(joining->pulseCount(), 1U);
    EXPECT_EQ(moving->pulseCount(), 100U);
    std::uint64_t expected{ 100U };
    for (const auto& edge : hardware->edges) {
        if (edge.axis == 0U && edge.high) {
            EXPECT_EQ(edge.tick, expected);
            expected += 200U;
        }
    }
    EXPECT_TRUE(hardware->registers.running);
}

TEST_F(StepTest, AxisCompletionCallbackIsOncePerRunAndIndependentOfProgressObserver)
{
    std::array<unsigned, 3> completions{};
    std::array<hal::step::AxisStatus, 3> final{};
    unsigned progress{};
    ASSERT_TRUE(generator->setProgressCallback([&](const auto&) noexcept { ++progress; }));
    for (unsigned i = 0; i < 3; ++i) {
        ASSERT_TRUE(outputs[i]->setCompletionCallback([&, i](const auto& status) noexcept {
            ++completions[i];
            final[i] = status;
        }));
        ASSERT_TRUE(outputs[i]->prepare({ 10us, 5us }, 600U + i));
    }
    ASSERT_TRUE(generator->start());
    ASSERT_TRUE(outputs[0]->start(10us));
    ASSERT_TRUE(outputs[1]->start(10us));
    hardware->advance(10'000U);
    EXPECT_EQ(completions, (std::array<unsigned, 3>{}));
    static_cast<void>(outputs[1]->stop());
    EXPECT_EQ(completions[1], 1U);
    EXPECT_EQ(final[1].state, State::Stopped);
    hardware->advance(100'000U);
    EXPECT_EQ(completions, (std::array<unsigned, 3>{ 1, 1, 0 }));
    EXPECT_EQ(final[0].state, State::Completed);
    EXPECT_EQ(final[0].pulses, 600U);
    EXPECT_GT(progress, 1U);
    static_cast<void>(outputs[0]->status());
    static_cast<void>(outputs[0]->stop());
    EXPECT_EQ(completions[0], 1U);
    ASSERT_TRUE(outputs[0]->start(10us));
    hardware->advance(100'000U);
    EXPECT_EQ(completions[0], 2U);
}

TEST_F(StepTest, CompletionSubscriptionBelongsToItsViewAndCanDetachWhileOtherAxesRun)
{
    unsigned calls{};
    auto observer{ generator->output(Axis::_1) };
    ASSERT_TRUE(observer->setCompletionCallback([&](const auto&) noexcept { ++calls; }));
    EXPECT_FALSE(outputs[0]->setCompletionCallback([](const auto&) noexcept {}));
    // A different view cannot erase the owner's subscription.
    EXPECT_FALSE(outputs[0]->setCompletionCallback({}));
    ASSERT_TRUE(generator->start());
    ASSERT_TRUE(outputs[0]->prepare({ 10us, 5us }, 2));
    ASSERT_TRUE(outputs[0]->start(10us));
    EXPECT_FALSE(observer->setCompletionCallback([](const auto&) noexcept {}));
    observer.reset(); // Synchronizes with dispatch and removes its capture.
    hardware->advance(1000U);
    EXPECT_EQ(calls, 0U);
    ASSERT_TRUE(outputs[0]->setCompletionCallback([&](const auto&) noexcept { ++calls; }));
    ASSERT_TRUE(outputs[0]->start(10us));
    hardware->advance(1000U);
    EXPECT_EQ(calls, 1U);
    ASSERT_TRUE(outputs[0]->setCompletionCallback({}));
    ASSERT_TRUE(outputs[0]->start(10us));
    hardware->advance(1000U);
    EXPECT_EQ(calls, 1U);
}

TEST_F(StepTest, FaultWakesEveryActiveAxisCompletionObserver)
{
    std::array<unsigned, 3> calls{};
    std::array<hal::step::AxisStatus, 3> final{};
    ASSERT_TRUE(generator->start());
    for (unsigned i = 0; i < 3; ++i) {
        ASSERT_TRUE(outputs[i]->setCompletionCallback([&, i](const auto& status) noexcept {
            ++calls[i];
            final[i] = status;
        }));
        ASSERT_TRUE(outputs[i]->prepare({ 10us, 5us }));
        ASSERT_TRUE(outputs[i]->start(10us));
    }
    hardware->advance(1000U);
    hardware->registers.error = true;
    generator->service();
    for (unsigned i = 0; i < 3; ++i) {
        EXPECT_EQ(calls[i], 1U);
        EXPECT_EQ(final[i].state, State::DmaError);
        EXPECT_FALSE(final[i].counts_exact);
    }
    generator->service();
    EXPECT_EQ(calls, (std::array<unsigned, 3>{ 1, 1, 1 }));
}
