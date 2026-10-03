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
    ASSERT_TRUE(generator->start(10us));
    hardware->advance(hal::detail::step_park * 3ULL);
    EXPECT_EQ(rises(0), (std::vector<std::uint64_t>{ 100, 1100, 2100 }));
    EXPECT_FALSE(hardware->high[0]);
    EXPECT_FALSE(hardware->registers.running);
    auto status{ generator->status() };
    EXPECT_EQ(status.state, State::Completed);
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
    ASSERT_TRUE(generator->start(1ms));
    hardware->advance(3'000'000U);
    const auto status{ generator->status() };
    EXPECT_EQ(status.state, State::Completed);
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
    ASSERT_TRUE(generator->start(10us));
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
    EXPECT_EQ(generator->status().state, State::Completed);
}

TEST_F(StepTest, MissedRefillStopsHardwareBeforeReplayingAStaleBuffer)
{
    ASSERT_TRUE(outputs[0]->prepare({ 10us, 5us }));
    ASSERT_TRUE(outputs[1]->prepare({ 31us, 9us }));
    ASSERT_TRUE(generator->start(10us));
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
    ASSERT_TRUE(generator->start(10us));
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
    ASSERT_TRUE(generator->start(10us));
    hardware->advance(expected_time + 100U);
    EXPECT_EQ(rises(0), expected);
    EXPECT_EQ(generator->status().state, State::Completed);
}

TEST_F(StepTest, CounterWrapDoesNotChangeSpacingAndParkedAxesStayLow)
{
    ASSERT_TRUE(outputs[0]->prepare({ 17s, 5us }, 70));
    ASSERT_TRUE(outputs[1]->prepare({ 1s, 5us }, 429));
    ASSERT_TRUE(outputs[2]->prepare({ 1s, 5us }, 1));
    ASSERT_TRUE(generator->start(10us));
    hardware->advance(12'000'000'000ULL);
    EXPECT_EQ(generator->status().state, State::Completed);
    ASSERT_EQ(rises(0).size(), 70U);
    ASSERT_EQ(rises(1).size(), 429U);
    ASSERT_EQ(rises(2).size(), 1U);
    const auto observed{ rises(0) };
    for (std::size_t i = 1; i < observed.size(); ++i) {
        EXPECT_EQ(observed[i] - observed[i - 1], 170'000'000ULL);
    }
}

TEST_F(StepTest, ConfigurationAndRestartAreAtomicAndDoNotStartUnpreparedAxes)
{
    ASSERT_FALSE(generator->start(10us));
    EXPECT_EQ(generator->output(static_cast<Axis>(255)), nullptr);
    ASSERT_TRUE(outputs[0]->prepare({ 100us, 5us }, 2));
    EXPECT_FALSE(outputs[0]->prepare({ 1us, 1us }, 5));
    EXPECT_FALSE(outputs[0]->prepare({ 100us, 5us }, 0));
    EXPECT_FALSE(outputs[0]->prepareSequence({}));
    EXPECT_FALSE(generator->start(0ns));
    ASSERT_TRUE(generator->start(10us));
    EXPECT_FALSE(generator->start(10us));
    EXPECT_FALSE(outputs[1]->prepare({ 100us, 5us }, 1));
    EXPECT_FALSE(outputs[0]->clear());
    hardware->advance(3000U);
    EXPECT_EQ(generator->status().state, State::Completed);
    EXPECT_EQ(rises(1).size(), 0U);
    ASSERT_TRUE(generator->start(10us));
    EXPECT_EQ(outputs[0]->pulseCount(), 0U);
    hardware->advance(3000U);
    EXPECT_EQ(generator->status().pulses[0], 2U);
    ASSERT_TRUE(outputs[0]->clear());
    EXPECT_FALSE(generator->start(10us));
}

TEST_F(StepTest, ErrorDoesNotClaimAnExactPositionOrRestartMotion)
{
    ASSERT_TRUE(outputs[0]->prepare({ 100us, 5us }));
    ASSERT_TRUE(generator->start(10us));
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
        EXPECT_FALSE(generator->start(10us));
    }));
    ASSERT_TRUE(outputs[0]->prepare({ 100us, 5us }, 1300));
    ASSERT_TRUE(generator->start(10us));
    hardware->advance(2'000'000U);
    EXPECT_GT(callbacks, 1U);
    EXPECT_LT(callbacks, 20U);
    EXPECT_EQ(generator->status().state, State::Completed);
}

TEST_F(StepTest, LargeTargetsUseBoundedBuffersAndContinuousOperationCanBeAborted)
{
    ASSERT_TRUE(outputs[0]->prepare({ 10us, 5us }, std::numeric_limits<std::uint64_t>::max()));
    ASSERT_TRUE(outputs[1]->prepare({ 20us, 5us }));
    ASSERT_TRUE(generator->start(10us));
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
        ASSERT_TRUE(generator->start(10us));
        hardware->advance(200'000U);
        EXPECT_EQ(generator->status().state, State::Completed);
        EXPECT_EQ(rises(0).size(), count);
        EXPECT_EQ(outputs[0]->pulseCount(), count);
        EXPECT_FALSE(hardware->high[0]);
    }
}

TEST_F(StepTest, PendingTerminalDmaCannotLeaveAnOldCompareThatRepeatsAfterWrap)
{
    ASSERT_TRUE(outputs[0]->prepare({ 100us, 5us }, 1));
    ASSERT_TRUE(outputs[1]->prepare({ 17s, 5us }, 30));
    ASSERT_TRUE(generator->start(10us));
    hardware->advance(100U); // First rise loaded its falling timestamp.
    hardware->hold_dma[0] = true;
    hardware->advance(50U); // Last fall happened, terminal DMA still pending.
    generator->service();
    EXPECT_EQ(outputs[0]->pulseCount(), 1U);
    hardware->advance(6'000'000'000ULL);
    EXPECT_EQ(generator->status().state, State::Completed);
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
    auto axis{ engine->output(Axis::M1) };
    ASSERT_TRUE(axis->prepare({ 10us, 5us }));
    ASSERT_TRUE(engine->start(10us));
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
    ASSERT_TRUE(generator->start(10us));
    hardware->advance(100U);
    hardware->hold_dma[0] = true;
    hardware->advance(1100U); // Last fall occurred; next rising timestamp was not loaded.
    hardware->hold_dma[0] = false;
    hardware->transfer(0U); // Loads an already missed rising compare.
    EXPECT_EQ(rises(0).size(), 1U);
    EXPECT_EQ(outputs[0]->pulseCount(), 1U);
    hardware->advance(20'000U);
    EXPECT_EQ(generator->status().state, State::Underrun);
    EXPECT_EQ(outputs[0]->pulseCount(), 1U);
}
