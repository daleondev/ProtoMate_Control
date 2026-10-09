#include "hal/board/board.hpp"
#include "hal/devices/impl/QuadratureEncoderFeedback.hpp"
#include "hal/drivers/impl/linux/Gpio.hpp"
#include "hal/drivers/impl/linux/QuadratureEncoder.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace
{
    using namespace pnm::units::literals;
    using namespace std::chrono_literals;
    using Feedback = hal::device::IMotorFeedback;

    // Deterministic timestamps/counts isolate feedback math from host scheduling.
    class Encoder final : public hal::IQuadratureEncoder
    {
      public:
        hal::util::Result<> start() noexcept override { value.running = true; publish(); return {}; }
        hal::util::Result<> stop() noexcept override { value.running = false; publish(); return {}; }
        bool isRunning() const noexcept override { return value.running; }
        hal::util::Result<Count> position() const noexcept override { return value.position; }
        hal::util::Result<> setPosition(Count count) noexcept override
        {
            value.position = count;
            ++resets;
            return {};
        }
        void setSampleCallback(SampleCallback next) override { callback = std::move(next); publish(); }
        void publish() { if (callback) callback(value); }
        void emit(Count count, std::chrono::nanoseconds time)
        {
            value.position = count;
            value.timestamp = time;
            publish();
        }
        Sample value{ 0, 0ns, false };
        SampleCallback callback;
        unsigned resets{};
    };

    class QuadratureFeedbackTest : public testing::Test
    {
      protected:
        std::shared_ptr<Encoder> encoder{ std::make_shared<Encoder>() };
        hal::device::QuadratureEncoderFeedback feedback{ encoder, 1600U };
        Feedback::Sample sample{};
        void SetUp() override
        {
            feedback.setCallback([this](const auto& value) noexcept { sample = value; });
            ASSERT_TRUE(feedback.start());
        }
        void TearDown() override { feedback.clearCallback(); }
    };
}

TEST_F(QuadratureFeedbackTest, ReportsShaftUnitsFromMeasuredCountsAndTimestamps)
{
    EXPECT_EQ(feedback.source(), Feedback::Source::ShaftEncoder);
    EXPECT_EQ(feedback.resolution(), 0.225_deg);
    encoder->emit(1600, 20ms);
    ASSERT_TRUE(sample.position);
    EXPECT_NEAR(sample.position->get<pnm::units::AngleUnits::deg>(), 360, 1e-9);
    EXPECT_NEAR(sample.velocity.get<pnm::units::AngularVelocityUnits::rpm>(), 3000, 1e-8);
    encoder->emit(-800, 60ms);
    EXPECT_NEAR(sample.position->get<pnm::units::AngleUnits::deg>(), -180, 1e-9);
    EXPECT_NEAR(sample.velocity.get<pnm::units::AngularVelocityUnits::rpm>(), -2250, 1e-8);
    encoder->emit(-800, 80ms);
    EXPECT_EQ(sample.velocity, 0_rpm);
    EXPECT_FALSE(sample.reference_lost);
}

TEST_F(QuadratureFeedbackTest, RebasePreservesRunningCounterAndDoesNotCreateVelocity)
{
    encoder->emit(792, 20ms);
    ASSERT_TRUE(feedback.reference(42.1_deg));
    ASSERT_TRUE(sample.position);
    EXPECT_NEAR(sample.position->get<pnm::units::AngleUnits::deg>(), 42.1, 1e-9);
    EXPECT_EQ(sample.velocity, 0_rpm);
    EXPECT_TRUE(encoder->isRunning());
    EXPECT_EQ(encoder->position(), 792);
    EXPECT_EQ(encoder->resets, 0U);
    encoder->emit(752, 40ms);
    EXPECT_NEAR(sample.position->get<pnm::units::AngleUnits::deg>(), 33.1, 1e-9);
    EXPECT_NEAR(sample.velocity.get<pnm::units::AngularVelocityUnits::rpm>(), -75, 1e-9);
}

TEST_F(QuadratureFeedbackTest, DriverMotionAndInvalidationDoNotReplaceShaftMeasurement)
{
    feedback.motion(true, true, 100us);
    encoder->emit(-160, 20ms); // Shaft can disagree with the commanded direction.
    EXPECT_EQ(sample.position, -36_deg);
    EXPECT_LT(sample.velocity, 0_rpm);
    feedback.motion(false, true, 0ns);
    feedback.invalidate(); // Losing driver phase does not lose the independent counter.
    encoder->emit(-320, 40ms);
    EXPECT_EQ(sample.position, -72_deg);
    EXPECT_TRUE(encoder->isRunning());
    ASSERT_TRUE(feedback.stop());
    EXPECT_EQ(sample.velocity, 0_rpm);
}

TEST_F(QuadratureFeedbackTest, CounterLossInvalidatesReferenceAndRejectsRebase)
{
    encoder->value.position = std::unexpected(std::make_error_code(std::errc::state_not_recoverable));
    encoder->publish();
    EXPECT_FALSE(sample.position);
    EXPECT_TRUE(sample.reference_lost);
    EXPECT_EQ(sample.velocity, 0_rpm);
    EXPECT_FALSE(feedback.reference(10_deg));
    EXPECT_TRUE(bool(encoder->callback)); // Failure must restore the subscription.
}

TEST_F(QuadratureFeedbackTest, SignedCountDifferenceDoesNotOverflow)
{
    encoder->emit(std::numeric_limits<std::int64_t>::min(), 20ms);
    encoder->emit(std::numeric_limits<std::int64_t>::max(), 40ms);
    EXPECT_TRUE(sample.position);
    EXPECT_TRUE(sample.velocity.isFinite());
    EXPECT_GT(sample.velocity, 0_rpm);
    encoder->emit(std::numeric_limits<std::int64_t>::min(), 60ms);
    EXPECT_LT(sample.velocity, 0_rpm);
}

TEST(QuadratureFeedback, DestructionDisconnectsAndStopsTheUnderlyingCounter)
{
    auto encoder{ std::make_shared<Encoder>() };
    unsigned publications{};
    {
        hal::device::QuadratureEncoderFeedback feedback{ encoder, 1600U };
        feedback.setCallback([&](const auto&) noexcept { ++publications; });
        ASSERT_TRUE(feedback.start());
    }
    EXPECT_FALSE(encoder->isRunning());
    EXPECT_FALSE(bool(encoder->callback));
    const auto before{ publications };
    encoder->emit(1600, 20ms);
    EXPECT_EQ(publications, before);
}

TEST(MotorFeedbackBoard, FactorySelectsQuadratureAndReservesItsInputs)
{
    using enum hal::board::MotorId;
    auto feedback{ hal::board::createMotorFeedback(Motor1, { 1.8_deg, 8U }) };
    ASSERT_TRUE(feedback);
    EXPECT_EQ(feedback->source(), Feedback::Source::ShaftEncoder);
    EXPECT_EQ(feedback->resolution(), 0.225_deg);
    auto encoder{ hal::encoder::simulatedEncoder(hal::timer::Peripheral::Tim3) };
    ASSERT_TRUE(encoder);
    EXPECT_FALSE(encoder->isRunning());
    EXPECT_FALSE(hal::board::createEncoderIndex(Motor1));
    EXPECT_FALSE(hal::board::createMotorFeedback(Motor1, { 1.8_deg, 16U }));
    ASSERT_TRUE(feedback->start());
    EXPECT_TRUE(encoder->isRunning());
    feedback.reset();
    EXPECT_FALSE(encoder->isRunning());
    EXPECT_TRUE(hal::board::createEncoderIndex(Motor1));
}

TEST(MotorFeedbackBoard, IndexUsesSuppliedMotorAngleAndKeepsUnknownFeedbackInvalid)
{
    using enum hal::board::MotorId;
    using enum hal::gpio::Level;
    for (auto id : { Motor2, Motor3 }) {
        auto feedback{ hal::board::createMotorFeedback(id, { 0.9_deg, 32U }) };
        ASSERT_TRUE(feedback);
        EXPECT_EQ(feedback->source(), Feedback::Source::DriverIndex);
        EXPECT_EQ(feedback->resolution(), 3.6_deg);
        Feedback::Sample sample;
        feedback->setCallback([&](const auto& value) noexcept { sample = value; });
        ASSERT_TRUE(feedback->reference(42_deg));
        EXPECT_FALSE(sample.position);
        ASSERT_TRUE(feedback->start());
        auto input{ hal::gpio::simulatedInput({ hal::gpio::Port::D, std::uint8_t(id == Motor2 ? 0 : 1) }) };
        ASSERT_TRUE(input);
        feedback->motion(true, true, 1ms);
        input->setSimulatedLevel(High);
        ASSERT_TRUE(sample.position);
        EXPECT_EQ(*sample.position, 42_deg);
        input->setSimulatedLevel(Low);
        input->setSimulatedLevel(High);
        EXPECT_NEAR(sample.position->get<pnm::units::AngleUnits::deg>(), 45.6, 1e-9);
        feedback->invalidate();
        EXPECT_FALSE(sample.position);
        EXPECT_FALSE(sample.reference_lost);
        feedback->clearCallback();
    }
}

TEST(MotorFeedbackBoard, RejectsInvalidMotorScalingBeforeClaimingHardware)
{
    using enum hal::board::MotorId;
    EXPECT_THROW(static_cast<void>(hal::board::createMotorFeedback(Motor1, { 0_deg, 16U })), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(hal::board::createMotorFeedback(Motor2, { 1.8_deg, 0U })), std::invalid_argument);
    EXPECT_FALSE(hal::board::createMotorFeedback(static_cast<hal::board::MotorId>(99), { 1.8_deg, 16U }));
    EXPECT_TRUE(hal::board::createMotorFeedback(Motor1, { 1.8_deg, 16U }));
}
