#include "hal/board/board.hpp"
#include "hal/drivers/detail/EncoderCounter.hpp"
#include "hal/drivers/detail/EncoderSampler.hpp"
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
#include <vector>

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

TEST(HalEncoder, SampleCadenceCoalescesDelayedServiceAndCanBeUnsubscribed)
{
    hal::detail::EncoderSampler sampler;
    std::vector<hal::IQuadratureEncoder::Sample> samples;
    sampler.setCallback([&](const auto& sample) noexcept { samples.push_back(sample); });
    sampler.publish({ 12, 100ms, true });
    EXPECT_FALSE(sampler.due(109ms));
    ASSERT_TRUE(sampler.due(110ms));
    sampler.publish({ 14, 110ms, true });
    ASSERT_TRUE(sampler.due(155ms));
    sampler.publish({ 30, 155ms, true });
    ASSERT_EQ(samples.size(), 3U);
    EXPECT_EQ(samples.back().timestamp - samples[1].timestamp, 45ms);
    EXPECT_EQ(samples.back().position, 30);
    EXPECT_FALSE(sampler.due(164ms));
    sampler.setCallback({});
    EXPECT_FALSE(sampler.due(1000ms));
    sampler.publish({ 99, 1000ms, true });
    EXPECT_EQ(samples.size(), 3U);
}

TEST(HalEncoder, SamplesAtRestWithoutReadsAndClearingDisconnectsCallbacks)
{
    const auto input{ hal::encoder::create(encoder) };
    ASSERT_NE(input, nullptr);
    std::atomic_uint samples{};
    std::atomic_bool was_running{};
    std::atomic<hal::IQuadratureEncoder::Count> position{};
    input->setSampleCallback([&](const auto& sample) noexcept {
        position.store(sample.position.value_or(-1));
        was_running.store(sample.running);
        samples.fetch_add(1U);
    });
    EXPECT_EQ(samples.load(), 1U); // Initial stopped snapshot.
    ASSERT_TRUE(input->start());
    const auto baseline{ samples.load() };
    const auto deadline{ std::chrono::steady_clock::now() + 500ms };
    while (samples.load() < baseline + 2U && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    EXPECT_GE(samples.load(), baseline + 2U);
    EXPECT_TRUE(was_running.load());
    EXPECT_EQ(position.load(), 0);
    ASSERT_TRUE(input->stop());
    EXPECT_FALSE(was_running.load());
    input->clearSampleCallback();
    const auto cleared{ samples.load() };
    ASSERT_TRUE(input->start());
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(samples.load(), cleared);
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
    const auto enable_n{ hal::board::createSteppersEnableOutput() };
    const auto first_dir{ hal::board::createStepperDirectionOutput(Motor1) };
    const auto second_dir{ hal::board::createStepperDirectionOutput(Motor2) };
    const auto third_dir{ hal::board::createStepperDirectionOutput(Motor3) };
    ASSERT_NE(enable_n, nullptr);
    ASSERT_NE(first_dir, nullptr);
    ASSERT_NE(second_dir, nullptr);
    ASSERT_NE(third_dir, nullptr);
    EXPECT_EQ(enable_n->read(), hal::gpio::Level::High);
    EXPECT_EQ(first_dir->read(), hal::gpio::Level::Low);
    EXPECT_EQ(second_dir->read(), hal::gpio::Level::Low);
    EXPECT_EQ(third_dir->read(), hal::gpio::Level::Low);
    first_dir->write(hal::gpio::Level::High);
    EXPECT_EQ(second_dir->read(), hal::gpio::Level::Low);
    EXPECT_EQ(third_dir->read(), hal::gpio::Level::Low);
    EXPECT_EQ(enable_n->read(), hal::gpio::Level::High);
    EXPECT_EQ(hal::board::createSteppersEnableOutput(), nullptr);
    EXPECT_EQ(hal::board::createStepperDirectionOutput(Motor1), nullptr);
    EXPECT_EQ(first_dir->read(), hal::gpio::Level::High);
    const auto generator{ hal::board::createStepperGenerator() };
    ASSERT_NE(generator, nullptr);
    EXPECT_EQ(hal::board::createStepperGenerator(), nullptr);
    const auto first{ hal::board::createStepperStepOutput(generator, Motor1) };
    const auto second{ hal::board::createStepperStepOutput(generator, Motor2) };
    const auto third{ hal::board::createStepperStepOutput(generator, Motor3) };
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(third, nullptr);
    EXPECT_EQ(generator->status().state, hal::step::State::Idle);
    ASSERT_TRUE(first->prepare({ 100us, 5us }, 3));
    ASSERT_TRUE(second->prepare({ 200us, 5us }, 2));
    ASSERT_TRUE(generator->start());
    ASSERT_TRUE(first->start());
    ASSERT_TRUE(second->start());
    EXPECT_FALSE(second->prepare({ 200us, 5us }, 2));
    EXPECT_EQ(third->pulseCount(), 0U);
    static_cast<void>(generator->stop());
    EXPECT_NE(hal::timer::create(5U), nullptr); // Runtime is independent of TIM2.
    EXPECT_EQ(hal::timer::create(2U), nullptr);
    EXPECT_EQ(hal::board::createEncoder(Motor2), nullptr);
    EXPECT_EQ(hal::board::createEncoderIndex(Motor3), nullptr);
    EXPECT_EQ(hal::board::createStepperStepOutput(generator, static_cast<hal::board::MotorId>(255)), nullptr);
    EXPECT_EQ(hal::board::createStepperDirectionOutput(static_cast<hal::board::MotorId>(255)), nullptr);

    const auto input{ std::dynamic_pointer_cast<hal::QuadratureEncoder>(hal::board::createEncoder(Motor1)) };
    const auto index{ std::dynamic_pointer_cast<hal::GpioInput>(hal::board::createEncoderIndex(Motor1)) };
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

TEST(HalMotorBoard, SharedStepEngineRetainsAllPinsUntilItsLastViewIsReleased)
{
    auto blocker{ hal::gpio::createInput({ .pin = { B, 10U } }) };
    ASSERT_NE(blocker, nullptr);
    EXPECT_EQ(hal::board::createStepperGenerator(), nullptr);
    auto rolled_back{ hal::gpio::createOutput({ .pin = { A, 0U } }) };
    ASSERT_NE(rolled_back, nullptr);
    rolled_back.reset();
    blocker.reset();
    auto generator{ hal::board::createStepperGenerator() };
    ASSERT_NE(generator, nullptr);
    auto axis{ hal::board::createStepperStepOutput(generator, hal::board::MotorId::Motor1) };
    ASSERT_NE(axis, nullptr);
    for (const auto pin :
         std::array{ hal::gpio::Pin{ A, 0U }, hal::gpio::Pin{ B, 10U }, hal::gpio::Pin{ B, 11U } }) {
        EXPECT_EQ(hal::gpio::createOutput({ .pin = pin }), nullptr);
    }
    generator.reset();
    EXPECT_EQ(hal::board::createStepperGenerator(), nullptr);
    EXPECT_TRUE(axis->prepare({ 100us, 5us }, 1U));
    axis.reset();
    EXPECT_NE(hal::board::createStepperGenerator(), nullptr);
}

TEST(HalMotorBoard, LinuxStepSimulationCompletesAndReportsBatchedProgressWhenQueried)
{
    auto generator{ hal::board::createStepperGenerator() };
    ASSERT_NE(generator, nullptr);
    auto axis{ hal::board::createStepperStepOutput(generator, hal::board::MotorId::Motor1) };
    ASSERT_TRUE(axis->prepare({ 10us, 5us }, 600U));
    unsigned completions{};
    ASSERT_TRUE(generator->setProgressCallback([&](const auto& status) noexcept {
        if (status.axes[0] == hal::step::State::Completed) {
            ++completions;
        }
    }));
    ASSERT_TRUE(generator->start());
    ASSERT_TRUE(axis->start());
    std::this_thread::sleep_for(10ms);
    EXPECT_EQ(generator->status().state, hal::step::State::Running);
    EXPECT_EQ(axis->status().state, hal::step::State::Completed);
    EXPECT_EQ(axis->pulseCount(), 600U);
    EXPECT_EQ(completions, 1U);
}

TEST(HalMotorBoard, ReferenceSwitchesReportOpenContactsAndBothTransitionsIndependently)
{
    using enum hal::board::MotorId;
    using enum hal::gpio::Level;
    constexpr std::array motors{ Motor1, Motor2, Motor3 };
    std::array<std::shared_ptr<hal::GpioInput>, motors.size()> inputs;
    std::array<unsigned, motors.size()> events{};
    const auto index{ hal::board::createEncoderIndex(Motor1) };
    ASSERT_NE(index, nullptr);
    for (std::size_t i = 0; i < motors.size(); ++i) {
        inputs[i] =
          std::dynamic_pointer_cast<hal::GpioInput>(hal::board::createReferenceLimitSwitch(motors[i]));
        ASSERT_NE(inputs[i], nullptr);
        EXPECT_EQ(inputs[i]->read(), High); // Unconnected input is pulled high.
        EXPECT_EQ(hal::board::createReferenceLimitSwitch(motors[i]), nullptr);
        inputs[i]->setEdgeCallback([&, i](hal::gpio::Level) noexcept { ++events[i]; });
        inputs[i]->setSimulatedLevel(Low); // Connect the released NC contact.
    }
    for (std::size_t i = 0; i < motors.size(); ++i) {
        inputs[i]->setSimulatedLevel(High); // Actuate the switch / open the cable.
        inputs[i]->setSimulatedLevel(High); // No extra edge for an unchanged level.
        for (std::size_t j = 0; j < motors.size(); ++j) {
            EXPECT_EQ(inputs[j]->read(), i == j ? High : Low);
            EXPECT_EQ(events[j], j < i ? 3U : (j == i ? 2U : 1U));
        }
        inputs[i]->setSimulatedLevel(Low); // Release the switch again.
        EXPECT_EQ(events[i], 3U);
        inputs[i]->clearEdgeCallback();
    }
    inputs[0].reset();
    EXPECT_NE(hal::board::createReferenceLimitSwitch(Motor1), nullptr);
    EXPECT_EQ(hal::board::createReferenceLimitSwitch(static_cast<hal::board::MotorId>(255)), nullptr);
}

TEST(HalMotorBoard, CompletionArrivesWithoutAnyQueriesAndSubscriptionCanBeRetired)
{
    const auto generator{ hal::board::createStepperGenerator() };
    const auto axis{ generator->output(hal::step::Axis::_1) };
    ASSERT_TRUE(generator->start());
    std::atomic_uint completions{};
    ASSERT_TRUE(axis->setCompletionCallback([&](const auto& status) noexcept {
        if (status.state == hal::step::State::Completed) ++completions;
    }));
    ASSERT_TRUE(axis->prepare({ 10us, 5us }, 513U)); // Tail parks between DMA interrupt boundaries.
    ASSERT_TRUE(axis->start());
    std::this_thread::sleep_for(20ms); // No status/pulseCount/service calls.
    EXPECT_EQ(completions.load(), 1U);
    ASSERT_TRUE(axis->setCompletionCallback({}));
    ASSERT_TRUE(axis->prepare({ 10us, 5us }, 1U));
    ASSERT_TRUE(axis->start());
    std::this_thread::sleep_for(5ms);
    EXPECT_EQ(completions.load(), 1U);
}
