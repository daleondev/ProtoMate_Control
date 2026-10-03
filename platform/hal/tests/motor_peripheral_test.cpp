#include "hal/board/board.hpp"
#include "hal/drivers/detail/EncoderCounter.hpp"
#include "hal/drivers/detail/PwmTiming.hpp"
#include "hal/drivers/factory/encoder.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/factory/pwm.hpp"
#include "hal/drivers/factory/timer.hpp"
#include "hal/drivers/impl/linux/Gpio.hpp"
#include "hal/drivers/impl/linux/QuadratureEncoder.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <limits>
#include <memory>
#include <thread>

namespace
{
    using namespace std::chrono_literals;
    using enum hal::gpio::Port;
    constexpr hal::pwm::Configuration m1{ 1U, 1U, { E, 9U } };
    constexpr hal::encoder::Configuration encoder{ 3U, { B, 4U }, { B, 5U } };
}

TEST(HalPwm, StartsUnconfiguredAndRequiresStoppedConfiguration)
{
    const auto output{ hal::pwm::create(m1) };
    ASSERT_NE(output, nullptr);
    EXPECT_FALSE(output->isRunning());
    EXPECT_EQ(output->timing(), hal::IPwmOutput::Timing{});
    EXPECT_EQ(output->start().error(), std::errc::operation_not_permitted);
    ASSERT_TRUE(output->configure({ 100us, 3us }));
    EXPECT_EQ(output->timing(), (hal::IPwmOutput::Timing{ 100us, 3us }));
    ASSERT_TRUE(output->start());
    ASSERT_TRUE(output->start());
    EXPECT_TRUE(output->isRunning());
    EXPECT_EQ(output->configure({ 200us, 4us }).error(), std::errc::device_or_resource_busy);
    EXPECT_EQ(output->timing(), (hal::IPwmOutput::Timing{ 100us, 3us }));
    ASSERT_TRUE(output->stop());
    ASSERT_TRUE(output->stop());
    EXPECT_FALSE(output->isRunning());
    ASSERT_TRUE(output->configure({ 200us, 4us }));
    EXPECT_EQ(output->timing(), (hal::IPwmOutput::Timing{ 200us, 4us }));
}

TEST(HalPwm, ValidatesTimingWithoutDestroyingPriorConfiguration)
{
    const auto output{ hal::pwm::create(m1) };
    ASSERT_NE(output, nullptr);
    ASSERT_TRUE(output->configure({ 100us, 3us }));
    for (const auto timing :
         std::array<hal::IPwmOutput::Timing, 6U>{ { hal::IPwmOutput::Timing{ 0ns, 0ns },
                                                    { -1ns, 0ns },
                                                    { 100us, -1ns },
                                                    { 100us, 101us },
                                                    { std::chrono::nanoseconds::max(), 1us },
                                                    { 7ns, 6ns } } }) {
        EXPECT_FALSE(output->configure(timing));
        EXPECT_EQ(output->timing(), (hal::IPwmOutput::Timing{ 100us, 3us }));
    }
    ASSERT_TRUE(output->configure({ 100us, 0ns }));
    ASSERT_TRUE(output->start());
    EXPECT_TRUE(output->isRunning()); // A running timer can produce a constant low output.
    ASSERT_TRUE(output->stop());
    ASSERT_TRUE(output->configure({ 100us, 100us }));
    ASSERT_TRUE(output->start());
    ASSERT_TRUE(output->stop());
}

TEST(HalPwm, QuantizationCoversPrescalerBoundariesAndVerySlowPulses)
{
    const auto exact{ hal::detail::pwmTiming({ 100us, 3us }, 240'000'000U) };
    ASSERT_TRUE(exact);
    EXPECT_EQ(exact->prescaler, 0U);
    EXPECT_EQ(exact->period_ticks, 24'000U);
    EXPECT_EQ(exact->high_ticks, 720U);
    const auto prescaled{ hal::detail::pwmTiming({ 275us, 3us }, 240'000'000U) };
    ASSERT_TRUE(prescaled);
    EXPECT_EQ(prescaled->prescaler, 1U);
    EXPECT_EQ(prescaled->period_ticks, 33'000U);
    EXPECT_EQ(prescaled->actual, (hal::IPwmOutput::Timing{ 275us, 3us }));
    const auto rounded{ hal::detail::pwmTiming({ 101ns, 21ns }, 240'000'000U) };
    ASSERT_TRUE(rounded);
    EXPECT_EQ(rounded->actual.period, 105ns);
    EXPECT_EQ(rounded->actual.high_time, 25ns);
    const auto slow{ hal::detail::pwmTiming({ 17s, 5ms }, 240'000'000U) };
    ASSERT_TRUE(slow);
    EXPECT_LE(slow->prescaler, 65535U);
    EXPECT_LE(slow->period_ticks, 65535U);
    EXPECT_GE(slow->actual.period, 17s);
    EXPECT_FALSE(hal::detail::pwmTiming({ 18s, 5ms }, 240'000'000U));
    EXPECT_FALSE(hal::detail::pwmTiming({ 1s, 1ms }, 0U));
}

TEST(HalPwm, ValidatesRoutesAndReleasesTimerAfterPinClaimFailure)
{
    EXPECT_EQ(hal::pwm::create({ 2U, 1U, { E, 9U } }), nullptr);
    EXPECT_EQ(hal::pwm::create({ 1U, 2U, { E, 9U } }), nullptr);
    EXPECT_EQ(hal::pwm::create({ 1U, 1U, { E, 8U } }), nullptr);
    auto blocker{ hal::gpio::createInput({ .pin = m1.pin }) };
    ASSERT_NE(blocker, nullptr);
    EXPECT_EQ(hal::pwm::create(m1), nullptr);
    blocker.reset();
    auto output{ hal::pwm::create(m1) };
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(hal::pwm::create(m1), nullptr);
    EXPECT_EQ(hal::gpio::createOutput({ .pin = m1.pin }), nullptr);
    ASSERT_TRUE(output->configure({ 1ms, 5us }));
    ASSERT_TRUE(output->start());
    output.reset(); // Destruction must release a running peripheral as well.
    output = hal::pwm::create(m1);
    ASSERT_NE(output, nullptr);
    EXPECT_FALSE(output->isRunning());
}

TEST(HalPwm, ConcurrentCreationHasExactlyOneOwner)
{
    std::barrier rendezvous{ 4 };
    std::atomic_uint successes{};
    std::array<std::thread, 4U> threads;
    for (auto& thread : threads) {
        thread = std::thread{ [&] {
            rendezvous.arrive_and_wait();
            const auto output{ hal::pwm::create(m1) };
            if (output) {
                ++successes;
            }
            rendezvous.arrive_and_wait(); // Keep the winner alive until every attempt completes.
        } };
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(successes.load(), 1U);
    EXPECT_NE(hal::pwm::create(m1), nullptr);
}

TEST(HalEncoder, ExtendsMovementAcrossManyWrapsAndPreservesStoppedPosition)
{
    const auto base{ hal::encoder::create(encoder) };
    const auto input{ std::dynamic_pointer_cast<hal::QuadratureEncoder>(base) };
    ASSERT_NE(input, nullptr);
    EXPECT_FALSE(input->isRunning());
    EXPECT_EQ(input->position(), 0);
    ASSERT_TRUE(input->advanceSimulatedCounts(123));
    EXPECT_EQ(input->position(), 0);
    ASSERT_TRUE(input->start());
    ASSERT_TRUE(input->start());
    ASSERT_TRUE(input->advanceSimulatedCounts(400'000));
    EXPECT_EQ(input->position(), 400'000);
    ASSERT_TRUE(input->advanceSimulatedCounts(-900'000));
    EXPECT_EQ(input->position(), -500'000);
    EXPECT_EQ(input->setPosition(42).error(), std::errc::device_or_resource_busy);
    ASSERT_TRUE(input->stop());
    ASSERT_TRUE(input->advanceSimulatedCounts(99));
    EXPECT_EQ(input->position(), -500'000);
    ASSERT_TRUE(input->start());
    ASSERT_TRUE(input->advanceSimulatedCounts(1));
    EXPECT_EQ(input->position(), -499'999);
    ASSERT_TRUE(input->stop());
    ASSERT_TRUE(input->setPosition(1'000'000'000'000LL));
    ASSERT_TRUE(input->start());
    ASSERT_TRUE(input->advanceSimulatedCounts(-1));
    EXPECT_EQ(input->position(), 999'999'999'999LL);
}

TEST(HalEncoder, RejectsUnsupportedRoutesAndRollsBackPartialPinClaims)
{
    EXPECT_EQ(hal::encoder::create({ 2U, { B, 4U }, { B, 5U } }), nullptr);
    EXPECT_EQ(hal::encoder::create({ 3U, { B, 5U }, { B, 4U } }), nullptr);
    auto blocker{ hal::gpio::createOutput({ .pin = encoder.b }) };
    ASSERT_NE(blocker, nullptr);
    EXPECT_EQ(hal::encoder::create(encoder), nullptr);
    EXPECT_NE(hal::gpio::createInput({ .pin = encoder.a }), nullptr);
    blocker.reset();
    auto input{ hal::encoder::create(encoder) };
    ASSERT_NE(input, nullptr);
    EXPECT_EQ(hal::encoder::create(encoder), nullptr);
    EXPECT_EQ(hal::gpio::createInput({ .pin = encoder.a }), nullptr);
    EXPECT_EQ(hal::gpio::createInput({ .pin = encoder.b }), nullptr);
    ASSERT_TRUE(input->start());
    input.reset();
    EXPECT_NE(hal::encoder::create(encoder), nullptr);
}

TEST(HalEncoder, CountExtensionHandlesReversalsNearWrapWithoutDirectionGuessing)
{
    hal::detail::EncoderCounter counter;
    counter.sample(65535U);
    EXPECT_EQ(counter.position(), -1);
    counter.sample(0U);
    EXPECT_EQ(counter.position(), 0);
    counter.sample(65534U);
    EXPECT_EQ(counter.position(), -2);
    counter.sample(1U);
    EXPECT_EQ(counter.position(), 1);
    // Model a counter advancing through the fixed ISR markers without application reads.
    counter.reset(0, 0U);
    for (int turn = 0; turn < 100; ++turn) {
        counter.sample(0x5555U);
        counter.sample(0xAAAAU);
        counter.sample(0U);
    }
    EXPECT_EQ(counter.position(), 6'553'600);
    for (int turn = 0; turn < 200; ++turn) {
        counter.sample(65535U);
        counter.sample(0xAAAAU);
        counter.sample(0x5555U);
        counter.sample(0U);
    }
    EXPECT_EQ(counter.position(), -6'553'600);
}

TEST(HalEncoder, ReportsAmbiguousSamplesAndSignedOverflowUntilExplicitReset)
{
    hal::detail::EncoderCounter counter;
    counter.sample(32768U);
    EXPECT_EQ(counter.position().error(), std::errc::result_out_of_range);
    counter.sample(32769U);
    EXPECT_FALSE(counter.position());
    counter.reset(std::numeric_limits<std::int64_t>::max(), 32769U);
    counter.sample(32770U);
    EXPECT_EQ(counter.position().error(), std::errc::value_too_large);
    counter.reset(std::numeric_limits<std::int64_t>::min(), 0U);
    counter.sample(65535U);
    EXPECT_EQ(counter.position().error(), std::errc::value_too_large);

    const auto input{ std::dynamic_pointer_cast<hal::QuadratureEncoder>(hal::encoder::create(encoder)) };
    ASSERT_NE(input, nullptr);
    ASSERT_TRUE(input->setPosition(std::numeric_limits<std::int64_t>::max()));
    ASSERT_TRUE(input->start());
    EXPECT_FALSE(input->advanceSimulatedCounts(1));
    EXPECT_FALSE(input->position());
    ASSERT_TRUE(input->stop());
    EXPECT_FALSE(input->start());
    ASSERT_TRUE(input->reset());
    ASSERT_TRUE(input->start());
    ASSERT_TRUE(input->advanceSimulatedCounts(1));
    EXPECT_EQ(input->position(), 1);
}

TEST(HalMotorBoard, AllOutputsAreIndependentAndIndexDoesNotChangePosition)
{
    using enum hal::board::MotorId;
    const auto first{ hal::board::createStepperStepOutput(M1) };
    const auto second{ hal::board::createStepperStepOutput(M2) };
    const auto third{ hal::board::createStepperStepOutput(M3) };
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(third, nullptr);
    EXPECT_FALSE(first->isRunning());
    EXPECT_FALSE(second->isRunning());
    EXPECT_FALSE(third->isRunning());
    ASSERT_TRUE(first->configure({ 100us, 3us }));
    ASSERT_TRUE(second->configure({ 200us, 4us }));
    ASSERT_TRUE(first->start());
    EXPECT_FALSE(second->isRunning());
    EXPECT_EQ(second->timing().period, 200us);
    EXPECT_EQ(third->timing().period, 0ns);
    EXPECT_NE(hal::timer::create(2U), nullptr); // Existing runtime timer is independent.
    EXPECT_EQ(hal::board::createEncoder(M2), nullptr);
    EXPECT_EQ(hal::board::createEncoderIndex(M3), nullptr);
    EXPECT_EQ(hal::board::createStepperStepOutput(static_cast<hal::board::MotorId>(255)), nullptr);

    const auto input{ std::dynamic_pointer_cast<hal::QuadratureEncoder>(hal::board::createEncoder(M1)) };
    const auto index{ std::dynamic_pointer_cast<hal::GpioInput>(hal::board::createEncoderIndex(M1)) };
    ASSERT_NE(input, nullptr);
    ASSERT_NE(index, nullptr);
    ASSERT_TRUE(input->start());
    ASSERT_TRUE(input->advanceSimulatedCounts(37));
    unsigned events{};
    index->setEdgeCallback([&](hal::gpio::Level) noexcept { ++events; });
    index->setSimulatedLevel(hal::gpio::Level::High);
    index->setSimulatedLevel(hal::gpio::Level::Low);
    EXPECT_EQ(events, 1U);
    EXPECT_EQ(input->position(), 37);
}
