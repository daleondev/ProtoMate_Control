#include "../StepperMotor.hpp"
#include "../AxisConversion.hpp"
#include "hal/drivers/impl/linux/Gpio.hpp"
#include "hal/drivers/impl/linux/QuadratureEncoder.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>

namespace
{
    using enum hal::board::MotorId;
    using enum StepperMotor::Result;
    using namespace std::chrono_literals;
    using namespace pnm::units::literals;

    std::shared_ptr<hal::GpioInput> releasedReference(hal::board::MotorId id)
    {
        // The motor owns the input; simulate the external NC contact by pin.
        constexpr std::array pins{
            hal::gpio::Pin{ hal::gpio::Port::E, 7U },
            hal::gpio::Pin{ hal::gpio::Port::E, 8U },
            hal::gpio::Pin{ hal::gpio::Port::E, 10U }
        };
        auto input{ hal::gpio::simulatedInput(pins.at(static_cast<std::size_t>(id))) };
        if (!input)
            throw std::runtime_error("test reference input unavailable");
        input->setSimulatedLevel(hal::gpio::Level::Low); // Connected, released NC switch.
        return input;
    }

    bool running(const std::shared_ptr<hal::IStepOutput>& axis)
    {
        const auto deadline{ std::chrono::steady_clock::now() + 500ms };
        while (std::chrono::steady_clock::now() < deadline) {
            if (axis->status().state == hal::step::State::Running)
                return true;
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }
    StepperMotor::Result result(std::future<StepperMotor::Result>& motion)
    {
        EXPECT_EQ(motion.wait_for(2s), std::future_status::ready);
        return motion.get();
    }

    bool measuredAt(const StepperMotor& motor, pnm::units::Angle target)
    {
        const auto measured{ motor.actualPosition() };
        return measured && std::abs((*measured - target).get<pnm::units::AngleUnits::deg>()) < 1e-8;
    }

    template<typename Predicate>
    bool eventually(Predicate predicate)
    {
        const auto deadline{ std::chrono::steady_clock::now() + 500ms };
        do {
            if (predicate())
                return true;
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    StepperMotor::Result referenceMotor(StepperMotor& motor, const std::shared_ptr<hal::GpioInput>& input)
    {
        using enum hal::gpio::Level;
        auto referencing{ motor.reference(5_rpm, 0.5_rpm, 1_s) };
        if (input->read() == Low) {
            if (!eventually([&] { return motor.velocity() > 0_rpm; })) {
                motor.stopAndWait();
                return result(referencing);
            }
            input->setSimulatedLevel(High);
        }
        if (!eventually([&] { return motor.velocity() < 0_rpm; })) {
            motor.stopAndWait();
            return result(referencing);
        }
        input->setSimulatedLevel(Low);
        if (!eventually([&] { return motor.velocity() > 0_rpm; })) {
            motor.stopAndWait();
            return result(referencing);
        }
        EXPECT_NEAR(motor.velocity().get<pnm::units::AngularVelocityUnits::rpm>(), 0.5, 0.001);
        input->setSimulatedLevel(High);
        return result(referencing);
    }

    // Deterministically inject switch edges inside prepare/start, after the
    // motor's preceding input check. All step execution still uses the real HAL.
    class StartHookGenerator final : public hal::IStepGenerator
    {
        class Output final : public hal::IStepOutput
        {
          public:
            Output(std::shared_ptr<hal::IStepOutput> axis,
                   std::function<void()> prepare_hook,
                   std::function<void()> start_hook)
              : axis{ std::move(axis) }, prepare_hook{ std::move(prepare_hook) }, start_hook{ std::move(start_hook) }
            {
            }
            auto setCompletionCallback(CompletionCallback cb) -> hal::util::Result<> override
            { return axis->setCompletionCallback(std::move(cb)); }
            auto setProgressCallback(ProgressCallback cb) -> hal::util::Result<> override
            { return axis->setProgressCallback(std::move(cb)); }
            auto prepare(hal::step::Timing timing, std::optional<hal::step::PulseCount> count)
              -> hal::util::Result<> override
            {
                if (prepare_hook) prepare_hook();
                return axis->prepare(timing, count);
            }
            auto prepareSequence(std::span<const hal::step::Timing> timings) -> hal::util::Result<> override
            { return axis->prepareSequence(timings); }
            auto start(std::chrono::nanoseconds delay) noexcept -> hal::util::Result<> override
            {
                if (start_hook) start_hook();
                return axis->start(delay);
            }
            auto stop() noexcept -> hal::step::AxisStatus override { return axis->stop(); }
            auto status() noexcept -> hal::step::AxisStatus override { return axis->status(); }
            auto updateTiming(hal::step::Timing timing) noexcept -> hal::util::Result<hal::step::PulseCount> override
            { return axis->updateTiming(timing); }
            auto clear() noexcept -> hal::util::Result<> override { return axis->clear(); }
            auto pulseCount() noexcept -> hal::util::Result<hal::step::PulseCount> override
            { return axis->pulseCount(); }
          private:
            std::shared_ptr<hal::IStepOutput> axis;
            std::function<void()> prepare_hook, start_hook;
        };
      public:
        std::shared_ptr<hal::IStepGenerator> generator{ hal::board::createStepperGenerator() };
        std::function<void()> prepare_hook, start_hook;
        auto output(hal::step::Axis axis) -> std::shared_ptr<hal::IStepOutput> override
        { return std::make_shared<Output>(generator->output(axis), prepare_hook, start_hook); }
        auto start() noexcept -> hal::util::Result<> override { return generator->start(); }
        auto stop() noexcept -> hal::step::Status override { return generator->stop(); }
        auto status() noexcept -> hal::step::Status override { return generator->status(); }
        auto setProgressCallback(ProgressCallback cb) -> hal::util::Result<> override
        { return generator->setProgressCallback(std::move(cb)); }
    };
}

TEST(StepperMotor, RequiresExternalGeneratorStartAndReferenceBeforeAbsolutePosition)
{
    const auto generator{ hal::board::createStepperGenerator() };
    ASSERT_NE(generator, nullptr);
    StepperMotor motor{ Motor2, 0_deg, 1.8_deg, 16U, generator };
    const auto input{ releasedReference(Motor2) };
    const auto axis{ generator->output(hal::step::Axis::_2) };
    auto rejected{ motor.moveRel(90_deg, 300_rpm) };
    EXPECT_EQ(result(rejected), Rejected);
    EXPECT_EQ(*axis->pulseCount(), 0U);
    ASSERT_TRUE(generator->start());
    auto forward{ motor.moveRel(90_deg, 300_rpm) };
    EXPECT_EQ(result(forward), Completed);
    EXPECT_EQ(*axis->pulseCount(), 800U);
    EXPECT_NEAR(motor.position().get<pnm::units::AngleUnits::deg>(), 90.0, 1e-9);
    auto backward{ motor.moveRel(-45_deg, 300_rpm) };
    EXPECT_EQ(result(backward), Completed);
    EXPECT_EQ(*axis->pulseCount(), 400U);
    auto absolute{ motor.moveAbs(0_deg, 300_rpm) };
    EXPECT_EQ(result(absolute), Rejected);
    EXPECT_EQ(*axis->pulseCount(), 400U);
    EXPECT_FALSE(motor.isReferenced());
    EXPECT_EQ(referenceMotor(motor, input), Completed);
    EXPECT_TRUE(motor.isReferenced());
    auto homed_absolute{ motor.moveAbs(-45_deg, 300_rpm) };
    EXPECT_EQ(result(homed_absolute), Completed);
    EXPECT_EQ(*axis->pulseCount(), 400U);
    input->setSimulatedLevel(hal::gpio::Level::Low);
    auto origin{ motor.moveAbs(0_deg, 300_rpm) };
    EXPECT_EQ(result(origin), Completed);
    EXPECT_NEAR(motor.position().get<pnm::units::AngleUnits::deg>(), 0.0, 1e-9);
    EXPECT_EQ(generator->status().state, hal::step::State::Running);
}

TEST(StepperMotor, AxisConversionPreservesReferencingAndMovesAwayWithReversedJointCoordinates)
{
    const RotaryAxisConversion axis{ { .motor_revolutions_per_axis_revolution = 5.0,
                                      .direction = AxisDirection::OppositeToMotor,
                                      .motor_reference = 135_deg,
                                      .axis_reference = 10_deg } };
    const auto generator{ hal::board::createStepperGenerator() };
    StepperMotor motor{ Motor2, axis.configuration().motor_reference, 1.8_deg, 16U, generator };
    const auto input{ releasedReference(Motor2) };
    ASSERT_TRUE(generator->start());
    auto unreferenced{ motor.moveAbs(axis.toMotorPosition(28_deg), axis.toMotorSpeed(60_rpm)) };
    EXPECT_EQ(result(unreferenced), Rejected);
    ASSERT_EQ(referenceMotor(motor, input), Completed);
    EXPECT_NEAR(axis.toAxisPosition(motor.position()).get<pnm::units::AngleUnits::deg>(), 10.0, 1e-9);

    // Positive joint displacement is a backward motor move, away from the
    // still-active switch. The speed magnitude must remain positive.
    auto motion{ motor.moveAbs(axis.toMotorPosition(28_deg), axis.toMotorSpeed(60_rpm)) };
    EXPECT_EQ(result(motion), Completed);
    EXPECT_NEAR(motor.position().get<pnm::units::AngleUnits::deg>(), 45.0, 1e-9);
    EXPECT_NEAR(axis.toAxisPosition(motor.position()).get<pnm::units::AngleUnits::deg>(), 28.0, 1e-9);
    EXPECT_EQ(*generator->output(hal::step::Axis::_2)->pulseCount(), 800U);
    EXPECT_FALSE(motor.actualPosition()); // Conversion cannot supply missing encoder feedback.
}

TEST(StepperMotor, IndependentStopRestartVelocityAndTimeoutLeaveOtherMotorRunning)
{
    const auto generator{ hal::board::createStepperGenerator() };
    ASSERT_NE(generator, nullptr);
    StepperMotor first{ Motor2, 0_deg, 1.8_deg, 16U, generator };
    releasedReference(Motor2);
    StepperMotor second{ Motor3, 0_deg, 1.8_deg, 16U, generator };
    releasedReference(Motor3);
    const auto axis{ generator->output(hal::step::Axis::_2) };
    const auto other{ generator->output(hal::step::Axis::_3) };
    ASSERT_TRUE(generator->start());
    auto continuous{ first.move(StepperMotor::Direction::Forward, 300_rpm) };
    ASSERT_TRUE(running(axis));
    auto replacement{ second.move(StepperMotor::Direction::Backward, 300_rpm) };
    ASSERT_TRUE(running(other));
    auto updated{ first.setVelocity(600_rpm) };
    ASSERT_TRUE(updated);
    EXPECT_GT(*updated, 0U);
    EXPECT_FALSE(first.setVelocity(0_rpm));
    const auto before{ *axis->pulseCount() };
    second.stopAndWait();
    EXPECT_EQ(result(replacement), Stopped);
    const auto origin{ second.position() };
    auto finite{ second.moveRel(45_deg, 300_rpm) };
    EXPECT_EQ(result(finite), Completed);
    EXPECT_EQ(*other->pulseCount(), 400U);
    EXPECT_NEAR((second.position() - origin).get<pnm::units::AngleUnits::deg>(), 45.0, 1e-9);
    auto timeout{ second.move(StepperMotor::Direction::Forward, 300_rpm, 0.01_s) };
    EXPECT_EQ(result(timeout), TimedOut);
    EXPECT_EQ(axis->status().state, hal::step::State::Running);
    EXPECT_GT(*axis->pulseCount(), before);
    first.stopAndWait();
    EXPECT_EQ(result(continuous), Stopped);
    EXPECT_EQ(generator->status().state, hal::step::State::Running);
}

TEST(StepperMotor, EncoderTracksExternalShaftMotionWhileStepGeneratorIsStopped)
{
    const auto generator{ hal::board::createStepperGenerator() };
    const auto started_before{ std::chrono::steady_clock::now() };
    StepperMotor motor{ Motor1, 0_deg, 1.8_deg, 8U, generator };
    const auto encoder{ hal::encoder::simulatedEncoder(3U) };
    ASSERT_NE(encoder, nullptr);
    ASSERT_TRUE(encoder->isRunning());
    ASSERT_TRUE(motor.actualPosition());
    EXPECT_EQ(*motor.actualPosition(), 0_deg);
    EXPECT_EQ(*motor.actualVelocity(), 0_rpm);

    // A full measured shaft turn is 1600 counts, independent of microstepping.
    ASSERT_TRUE(encoder->advanceSimulatedCounts(1600));
    auto measured_velocity{ 0_rpm };
    ASSERT_TRUE(eventually([&] {
        if (!measuredAt(motor, 360_deg))
            return false;
        measured_velocity = motor.actualVelocity().value_or(0_rpm);
        return measured_velocity > 0_rpm;
    }));
    // The first measured displacement took at least a sample period and no
    // longer than construction-to-observation. Check the speed's magnitude,
    // allowing scheduler delays without assuming an exact host wake-up time.
    const pnm::units::Time maximum_elapsed{ std::chrono::steady_clock::now() - started_before };
    EXPECT_GE(measured_velocity, 360_deg / maximum_elapsed - 0.000001_rpm);
    EXPECT_LE(measured_velocity,
              360_deg / pnm::units::Time{ hal::IQuadratureEncoder::sample_period } + 0.000001_rpm);
    ASSERT_TRUE(eventually([&] { return motor.actualVelocity() == 0_rpm; }));
    EXPECT_EQ(motor.position(), 0_deg); // No commanded pulses were generated.

    ASSERT_TRUE(encoder->advanceSimulatedCounts(-2000));
    ASSERT_TRUE(eventually([&] { return measuredAt(motor, -90_deg); }));
    EXPECT_LT(*motor.actualVelocity(), 0_rpm);
    ASSERT_TRUE(eventually([&] { return motor.actualVelocity() == 0_rpm; }));
    motor.stopAndWait();
    EXPECT_TRUE(encoder->isRunning()); // Stopping STEP must not stop measurement.
    ASSERT_TRUE(encoder->advanceSimulatedCounts(1));
    ASSERT_TRUE(eventually([&] { return measuredAt(motor, -89.775_deg); }));
}

TEST(StepperMotor, MeasuredPositionDoesNotPretendCommandedMotionOccurred)
{
    const auto generator{ hal::board::createStepperGenerator() };
    StepperMotor motor{ Motor1, 0_deg, 1.8_deg, 8U, generator };
    releasedReference(Motor1);
    ASSERT_TRUE(generator->start());
    auto motion{ motor.moveRel(90_deg, 300_rpm) };
    EXPECT_EQ(result(motion), Completed);
    EXPECT_NEAR(motor.position().get<pnm::units::AngleUnits::deg>(), 90.0, 1e-9);
    EXPECT_EQ(motor.actualPosition(), 0_deg);
    EXPECT_EQ(motor.actualVelocity(), 0_rpm);
}

TEST(StepperMotor, MotorsWithoutEncodersReportFeedbackUnavailable)
{
    const auto generator{ hal::board::createStepperGenerator() };
    for (const auto id : { Motor2, Motor3 }) {
        StepperMotor motor{ id, 0_deg, 1.8_deg, 16U, generator };
        EXPECT_EQ(motor.actualPosition().error(), std::errc::no_such_device);
        EXPECT_EQ(motor.actualVelocity().error(), std::errc::no_such_device);
    }
}

TEST(StepperMotor, EncoderWrapsAndFaultsAreNotPresentedAsValidMeasuredMotion)
{
    const auto generator{ hal::board::createStepperGenerator() };
    StepperMotor motor{ Motor1, 0_deg, 1.8_deg, 8U, generator };
    const auto encoder{ hal::encoder::simulatedEncoder(3U) };
    ASSERT_NE(encoder, nullptr);
    ASSERT_TRUE(encoder->advanceSimulatedCounts(-800'000));
    ASSERT_TRUE(eventually([&] { return measuredAt(motor, -180000_deg); }));
    ASSERT_TRUE(encoder->advanceSimulatedCounts(1'600'000));
    ASSERT_TRUE(eventually([&] { return measuredAt(motor, 180000_deg); }));
    ASSERT_TRUE(encoder->stop());
    ASSERT_TRUE(encoder->setPosition(std::numeric_limits<hal::IQuadratureEncoder::Count>::max()));
    ASSERT_TRUE(encoder->start());
    EXPECT_FALSE(encoder->advanceSimulatedCounts(1));
    ASSERT_TRUE(eventually([&] { return !motor.actualPosition(); }));
    EXPECT_EQ(motor.actualPosition().error(), std::errc::state_not_recoverable);
    EXPECT_EQ(motor.actualVelocity().error(), std::errc::state_not_recoverable);
}

TEST(StepperMotor, DestructionDisconnectsEncoderCallbackAndStopsCounting)
{
    const auto generator{ hal::board::createStepperGenerator() };
    std::shared_ptr<hal::QuadratureEncoder> encoder;
    {
        StepperMotor motor{ Motor1, 0_deg, 1.8_deg, 8U, generator };
        encoder = hal::encoder::simulatedEncoder(3U);
        ASSERT_NE(encoder, nullptr);
        ASSERT_TRUE(encoder->advanceSimulatedCounts(160));
        ASSERT_TRUE(eventually([&] { return measuredAt(motor, 36_deg); }));
    }
    EXPECT_FALSE(encoder->isRunning());
    ASSERT_TRUE(encoder->start());
    ASSERT_TRUE(encoder->advanceSimulatedCounts(160));
    std::this_thread::sleep_for(30ms); // No callback into the destroyed motor.
    EXPECT_EQ(encoder->position(), 320);
    encoder.reset();
    StepperMotor replacement{ Motor1, 0_deg, 1.8_deg, 8U, generator };
    EXPECT_EQ(replacement.actualPosition(), 0_deg);
}

TEST(StepperMotor, ReferenceRebasesCommandedAndEncoderPositionsAndPreservesFutureFeedback)
{
    using enum hal::gpio::Level;
    const auto generator{ hal::board::createStepperGenerator() };
    // Not an integer encoder count: rebasing must use a coordinate offset.
    StepperMotor motor{ Motor1, 42.1_deg, 1.8_deg, 16U, generator };
    const auto input{ releasedReference(Motor1) };
    const auto encoder{ hal::encoder::simulatedEncoder(3U) };
    ASSERT_TRUE(encoder->advanceSimulatedCounts(800));
    ASSERT_TRUE(generator->start());
    auto referencing{ motor.reference(5_rpm, 0.5_rpm, 1_s) };
    ASSERT_TRUE(eventually([&] { return motor.velocity() > 0_rpm; }));
    EXPECT_FALSE(motor.isReferenced());
    EXPECT_FALSE(motor.setVelocity(100_rpm));
    auto forbidden{ motor.moveAbs(0_deg, 5_rpm) };
    EXPECT_EQ(result(forbidden), Rejected);
    EXPECT_EQ(referencing.wait_for(0ms), std::future_status::timeout); // Not cancelled.

    input->setSimulatedLevel(High);
    ASSERT_TRUE(eventually([&] { return motor.velocity() < 0_rpm; }));
    ASSERT_TRUE(encoder->advanceSimulatedCounts(-20));
    input->setSimulatedLevel(Low);
    ASSERT_TRUE(eventually([&] { return motor.velocity() > 0_rpm; }));
    EXPECT_NEAR(motor.velocity().get<pnm::units::AngularVelocityUnits::rpm>(), 0.5, 0.001);
    ASSERT_TRUE(encoder->advanceSimulatedCounts(12));
    input->setSimulatedLevel(High);
    EXPECT_EQ(result(referencing), Completed);
    ASSERT_TRUE(motor.isReferenced());
    EXPECT_NEAR(motor.position().get<pnm::units::AngleUnits::deg>(), 42.1, 1e-8);
    EXPECT_TRUE(measuredAt(motor, 42.1_deg));
    EXPECT_TRUE(encoder->isRunning());
    EXPECT_EQ(encoder->position(), 792); // Count was not stopped/reset.
    EXPECT_EQ(motor.velocity(), 0_rpm);
    std::this_thread::sleep_for(30ms);
    EXPECT_TRUE(measuredAt(motor, 42.1_deg)); // Periodic callback keeps the new origin.
    EXPECT_EQ(motor.actualVelocity(), 0_rpm);

    ASSERT_TRUE(encoder->advanceSimulatedCounts(-40));
    ASSERT_TRUE(eventually([&] { return measuredAt(motor, 33.1_deg); }));
    auto away{ motor.moveAbs(30.85_deg, 300_rpm) };
    EXPECT_EQ(result(away), Completed);
    EXPECT_NEAR(motor.position().get<pnm::units::AngleUnits::deg>(), 30.85, 1e-8);

    // A lost encoder count invalidates the reference and absolute eligibility.
    ASSERT_TRUE(encoder->stop());
    ASSERT_TRUE(encoder->setPosition(std::numeric_limits<hal::IQuadratureEncoder::Count>::max()));
    ASSERT_TRUE(encoder->start());
    EXPECT_FALSE(encoder->advanceSimulatedCounts(1));
    ASSERT_TRUE(eventually([&] { return !motor.isReferenced(); }));
    auto invalidated{ motor.moveAbs(0_deg, 5_rpm) };
    EXPECT_EQ(result(invalidated), Rejected);
}

TEST(StepperMotor, ReferenceStartingOnSwitchBacksAwayBeforeApproaching)
{
    const auto generator{ hal::board::createStepperGenerator() };
    StepperMotor motor{ Motor2, -12_deg, 1.8_deg, 16U, generator };
    const auto input{ hal::gpio::simulatedInput({ hal::gpio::Port::E, 8U }) };
    ASSERT_EQ(input->read(), hal::gpio::Level::High);
    ASSERT_TRUE(generator->start());
    EXPECT_EQ(referenceMotor(motor, input), Completed);
    EXPECT_TRUE(motor.isReferenced());
    EXPECT_EQ(motor.position(), -12_deg);
    EXPECT_EQ(motor.actualPosition().error(), std::errc::no_such_device);
    auto zero{ motor.moveAbs(-12_deg, 5_rpm) };
    EXPECT_EQ(result(zero), Completed);
}

TEST(StepperMotor, ReferenceMissingOrStuckSwitchTimesOutWithoutHoming)
{
    const auto generator{ hal::board::createStepperGenerator() };
    StepperMotor motor{ Motor2, 90_deg, 1.8_deg, 16U, generator };
    const auto input{ releasedReference(Motor2) };
    ASSERT_TRUE(generator->start());
    for (const auto level : { hal::gpio::Level::Low, hal::gpio::Level::High }) {
        input->setSimulatedLevel(level);
        auto homing{ motor.reference(5_rpm, 0.5_rpm, 0.05_s) };
        EXPECT_EQ(result(homing), TimedOut);
        EXPECT_FALSE(motor.isReferenced());
        EXPECT_EQ(motor.velocity(), 0_rpm);
        EXPECT_NE(motor.position(), 90_deg);
    }
}

TEST(StepperMotor, ReferenceCancelledInEveryPhaseLeavesAxisUnreferenced)
{
    using enum hal::gpio::Level;
    const auto generator{ hal::board::createStepperGenerator() };
    StepperMotor motor{ Motor2, 90_deg, 1.8_deg, 16U, generator };
    const auto input{ releasedReference(Motor2) };
    ASSERT_TRUE(generator->start());
    for (int phase = 0; phase < 3; ++phase) {
        input->setSimulatedLevel(Low);
        auto homing{ motor.reference() };
        ASSERT_TRUE(eventually([&] { return motor.velocity() > 0_rpm; }));
        if (phase >= 1) {
            input->setSimulatedLevel(High);
            ASSERT_TRUE(eventually([&] { return motor.velocity() < 0_rpm; }));
        }
        if (phase >= 2) {
            input->setSimulatedLevel(Low);
            ASSERT_TRUE(eventually([&] { return motor.velocity() > 0_rpm; }));
        }
        motor.stopAndWait();
        EXPECT_EQ(result(homing), Stopped);
        EXPECT_FALSE(motor.isReferenced());
        EXPECT_EQ(motor.velocity(), 0_rpm);
    }
}

TEST(StepperMotor, ReferenceRequiresStableSecondContactAndOneOverallTimeout)
{
    using enum hal::gpio::Level;
    const auto generator{ hal::board::createStepperGenerator() };
    StepperMotor motor{ Motor2, 90_deg, 1.8_deg, 16U, generator };
    const auto input{ releasedReference(Motor2) };
    ASSERT_TRUE(generator->start());
    const auto began{ std::chrono::steady_clock::now() };
    auto homing{ motor.reference(5_rpm, 0.5_rpm, 0.2_s) };
    ASSERT_TRUE(eventually([&] { return motor.velocity() > 0_rpm; }));
    input->setSimulatedLevel(High);
    ASSERT_TRUE(eventually([&] { return motor.velocity() < 0_rpm; }));
    input->setSimulatedLevel(Low);
    ASSERT_TRUE(eventually([&] { return motor.velocity() > 0_rpm; }));
    input->setSimulatedLevel(High);
    input->setSimulatedLevel(Low); // Brief contact must stop immediately, not establish home.
    ASSERT_TRUE(eventually([&] { return motor.velocity() == 0_rpm; }));
    EXPECT_FALSE(motor.isReferenced());
    EXPECT_EQ(result(homing), TimedOut);
    EXPECT_LT(std::chrono::steady_clock::now() - began, 300ms);
    EXPECT_NE(motor.position(), 90_deg);
}

TEST(StepperMotor, ReplacingReferenceInvalidatesOldHomeAndLeavesOtherAxisRunning)
{
    using enum hal::gpio::Level;
    const auto generator{ hal::board::createStepperGenerator() };
    StepperMotor motor{ Motor2, 0_deg, 1.8_deg, 16U, generator };
    const auto input{ releasedReference(Motor2) };
    StepperMotor other{ Motor3, 0_deg, 1.8_deg, 16U, generator };
    releasedReference(Motor3);
    ASSERT_TRUE(generator->start());
    auto independent{ other.move(StepperMotor::Direction::Backward, 5_rpm) };
    EXPECT_EQ(referenceMotor(motor, input), Completed);
    EXPECT_TRUE(motor.isReferenced());
    auto homing{ motor.reference() }; // Starts pressed, then moves backward.
    ASSERT_TRUE(eventually([&] { return motor.velocity() < 0_rpm; }));
    EXPECT_FALSE(motor.isReferenced());
    auto replacement{ motor.moveRel(-1.125_deg, 300_rpm) };
    EXPECT_EQ(result(homing), Stopped);
    EXPECT_EQ(result(replacement), Completed);
    EXPECT_FALSE(motor.isReferenced());
    EXPECT_LT(other.velocity(), 0_rpm);
    other.stopAndWait();
    EXPECT_EQ(result(independent), Stopped);
}

TEST(StepperMotor, ReferenceValidatesSpeedsTimeoutAndGeneratorState)
{
    const auto generator{ hal::board::createStepperGenerator() };
    EXPECT_THROW((StepperMotor{ Motor2, 1_deg * std::numeric_limits<double>::quiet_NaN(),
                                 1.8_deg, 16U, generator }), std::invalid_argument);
    StepperMotor motor{ Motor2, 0_deg, 1.8_deg, 16U, generator };
    releasedReference(Motor2);
    auto not_started{ motor.reference() };
    EXPECT_EQ(result(not_started), Rejected);
    ASSERT_TRUE(generator->start());
    auto invalid_speed{ motor.reference(5_rpm, 5_rpm) };
    EXPECT_EQ(result(invalid_speed), Rejected);
    auto zero_speed{ motor.reference(0_rpm, 0.5_rpm) };
    EXPECT_EQ(result(zero_speed), Rejected);
    auto unlimited{ motor.reference(5_rpm, 0.5_rpm, 0_s) };
    EXPECT_EQ(result(unlimited), Rejected);
    auto negative{ motor.reference(5_rpm, 0.5_rpm, -1_s) };
    EXPECT_EQ(result(negative), Rejected);
    EXPECT_FALSE(motor.isReferenced());
    EXPECT_EQ(motor.position(), 0_deg);
}

TEST(StepperMotor, ReplacingMoveJoinsAndAccountsPreviousMotionBeforeRelativeTarget)
{
    const auto generator{ hal::board::createStepperGenerator() };
    ASSERT_NE(generator, nullptr);
    StepperMotor motor{ Motor3, 0_deg, 1.8_deg, 16U, generator };
    releasedReference(Motor3);
    ASSERT_TRUE(generator->start());
    const auto axis{ generator->output(hal::step::Axis::_3) };
    auto old{ motor.move(StepperMotor::Direction::Forward, 300_rpm) };
    ASSERT_TRUE(running(axis));
    std::this_thread::sleep_for(10ms);
    auto next{ motor.moveRel(45_deg, 300_rpm) };
    EXPECT_EQ(result(old), Stopped);
    EXPECT_EQ(result(next), Completed);
    EXPECT_EQ(*axis->pulseCount(), 400U);
    EXPECT_GT(motor.position().get<pnm::units::AngleUnits::deg>(), 45.0);
}

TEST(StepperMotor, InvalidRequestsAndDestructionDoNotStopOtherAxis)
{
    const auto generator{ hal::board::createStepperGenerator() };
    ASSERT_NE(generator, nullptr);
    EXPECT_THROW((StepperMotor{ Motor3, 0_deg, 1.8_deg, 0U, generator }), std::invalid_argument);
    StepperMotor first{ Motor2, 0_deg, 1.8_deg, 16U, generator };
    releasedReference(Motor2);
    ASSERT_TRUE(generator->start());
    const auto axis{ generator->output(hal::step::Axis::_2) };
    auto continuous{ first.move(StepperMotor::Direction::Forward, 300_rpm) };
    ASSERT_TRUE(running(axis));
    std::future<StepperMotor::Result> destroyed;
    {
        StepperMotor second{ Motor3, 0_deg, 1.8_deg, 16U, generator };
        releasedReference(Motor3);
        auto bad{ second.moveRel(90_deg, 0_rpm) };
        EXPECT_EQ(result(bad), Rejected);
        auto negative{ second.moveRel(90_deg, -1_rpm) };
        EXPECT_EQ(result(negative), Rejected);
        auto nan{ second.moveAbs(1_deg * std::numeric_limits<double>::quiet_NaN(), 300_rpm) };
        EXPECT_EQ(result(nan), Rejected);
        destroyed = second.move(StepperMotor::Direction::Forward, 300_rpm);
        ASSERT_TRUE(running(generator->output(hal::step::Axis::_3)));
    }
    EXPECT_EQ(result(destroyed), Stopped);
    EXPECT_EQ(axis->status().state, hal::step::State::Running);
    first.stopAndWait();
    EXPECT_EQ(result(continuous), Stopped);
}

TEST(StepperMotor, CompletionAndCancellationWakeBlockedWorkersAcrossManyReplacements)
{
    const auto generator{ hal::board::createStepperGenerator() };
    ASSERT_NE(generator, nullptr);
    StepperMotor motor{ Motor2, 0_deg, 1.8_deg, 16U, generator };
    releasedReference(Motor2);
    ASSERT_TRUE(generator->start());
    for (unsigned i = 0; i < 30; ++i) {
        auto previous{ motor.move(StepperMotor::Direction::Forward, 300_rpm) };
        // Replace even when the prior worker has not reached its wait/start yet.
        auto next{ motor.moveRel(0.1125_deg, 300_rpm) };
        EXPECT_EQ(result(previous), Stopped);
        EXPECT_EQ(result(next), Completed);
    }
    auto moving{ motor.move(StepperMotor::Direction::Forward, 300_rpm) };
    ASSERT_TRUE(running(generator->output(hal::step::Axis::_2)));
    std::this_thread::sleep_for(10ms);
    const auto before{ motor.position() };
    std::this_thread::sleep_for(10ms);
    EXPECT_GT(motor.position(), before); // Position refreshes without waking the motion worker.
    motor.stopAndWait();
    EXPECT_EQ(result(moving), Stopped);
}

TEST(StepperMotor, GlobalStopWakesAllMotorsAndEachCanBeUsedAfterAnExplicitRestart)
{
    const auto generator{ hal::board::createStepperGenerator() };
    ASSERT_NE(generator, nullptr);
    StepperMotor first{ Motor2, 0_deg, 1.8_deg, 16U, generator };
    releasedReference(Motor2);
    StepperMotor second{ Motor3, 0_deg, 1.8_deg, 16U, generator };
    releasedReference(Motor3);
    ASSERT_TRUE(generator->start());
    auto a{ first.move(StepperMotor::Direction::Forward, 300_rpm) };
    auto b{ second.move(StepperMotor::Direction::Forward, 300_rpm) };
    ASSERT_TRUE(running(generator->output(hal::step::Axis::_2)));
    ASSERT_TRUE(running(generator->output(hal::step::Axis::_3)));
    static_cast<void>(generator->stop());
    EXPECT_EQ(result(a), Stopped);
    EXPECT_EQ(result(b), Stopped);
    ASSERT_TRUE(generator->start());
    auto next{ first.moveRel(1.125_deg, 300_rpm) };
    EXPECT_EQ(result(next), Completed);
}

TEST(StepperMotor, CurrentVelocityIsSignedAndChangesWhenQueuedTimingExecutes)
{
    const auto generator{ hal::board::createStepperGenerator() };
    ASSERT_NE(generator, nullptr);
    StepperMotor motor{ Motor2, 0_deg, 1.8_deg, 16U, generator };
    releasedReference(Motor2);
    const auto axis{ generator->output(hal::step::Axis::_2) };
    ASSERT_TRUE(generator->start());
    EXPECT_EQ(motor.velocity(), 0_rpm);
    // 2 ms per pulse leaves ample time to inspect a pending timing change.
    auto motion{ motor.move(StepperMotor::Direction::Backward, 9.375_rpm) };
    ASSERT_TRUE(running(axis));
    std::this_thread::sleep_for(10ms);
    EXPECT_LT(motor.position(), 0_deg);
    EXPECT_NEAR(motor.velocity().get<pnm::units::AngularVelocityUnits::rpm>(), -9.375, 0.001);
    const auto boundary{ motor.setVelocity(18.75_rpm) };
    ASSERT_TRUE(boundary);
    ASSERT_LT(*axis->pulseCount(), *boundary);
    EXPECT_NEAR(motor.velocity().get<pnm::units::AngularVelocityUnits::rpm>(), -9.375, 0.001);
    std::this_thread::sleep_for(1100ms);
    EXPECT_GE(*axis->pulseCount(), *boundary);
    EXPECT_NEAR(motor.velocity().get<pnm::units::AngularVelocityUnits::rpm>(), -18.75, 0.001);
    motor.stop(); // Final state is published synchronously, before joining the worker.
    const auto stopped{ motor.position() };
    EXPECT_EQ(motor.velocity(), 0_rpm);
    EXPECT_EQ(result(motion), Stopped);
    EXPECT_EQ(motor.position(), stopped);
    auto next{ motor.moveRel(1.125_deg, 300_rpm) };
    EXPECT_EQ(result(next), Completed);
    EXPECT_NEAR((motor.position() - stopped).get<pnm::units::AngleUnits::deg>(), 1.125, 1e-9);
    EXPECT_EQ(motor.velocity(), 0_rpm);
}

TEST(StepperMotor, OpenReferenceRejectsTowardMovesButAllowsAwayAndZeroDistance)
{
    const auto generator{ hal::board::createStepperGenerator() };
    ASSERT_NE(generator, nullptr);
    // Default board input is unconnected: pull-up HIGH, no activation edge needed.
    StepperMotor motor{ Motor2, 0_deg, 1.8_deg, 16U, generator };
    const auto axis{ generator->output(hal::step::Axis::_2) };
    ASSERT_TRUE(generator->start());
    auto forward{ motor.move(StepperMotor::Direction::Forward, 300_rpm) };
    EXPECT_EQ(result(forward), Rejected);
    auto relative{ motor.moveRel(1.125_deg, 300_rpm) };
    EXPECT_EQ(result(relative), Rejected);
    auto absolute{ motor.moveAbs(1.125_deg, 300_rpm) };
    EXPECT_EQ(result(absolute), Rejected);
    EXPECT_EQ(*axis->pulseCount(), 0U);
    auto unreferenced_zero{ motor.moveAbs(motor.position(), 300_rpm) };
    EXPECT_EQ(result(unreferenced_zero), Rejected);
    auto zero{ motor.moveRel(0_deg, 300_rpm) };
    EXPECT_EQ(result(zero), Completed);
    auto away{ motor.moveRel(-1.125_deg, 300_rpm) };
    EXPECT_EQ(result(away), Completed);
    EXPECT_EQ(*axis->pulseCount(), 10U);
    auto toward{ motor.moveAbs(0_deg, 300_rpm) };
    EXPECT_EQ(result(toward), Rejected);
    EXPECT_NEAR(motor.position().get<pnm::units::AngleUnits::deg>(), -1.125, 1e-9);
}

TEST(StepperMotor, ActivationStopsOnlyApproachingAxisAndReleaseDoesNotStopRetreat)
{
    using enum hal::gpio::Level;
    const auto generator{ hal::board::createStepperGenerator() };
    ASSERT_NE(generator, nullptr);
    StepperMotor motor{ Motor2, 0_deg, 1.8_deg, 16U, generator };
    const auto input{ releasedReference(Motor2) };
    StepperMotor other{ Motor3, 0_deg, 1.8_deg, 16U, generator };
    releasedReference(Motor3);
    const auto axis{ generator->output(hal::step::Axis::_2) };
    const auto other_axis{ generator->output(hal::step::Axis::_3) };
    ASSERT_TRUE(generator->start());
    auto motion{ motor.move(StepperMotor::Direction::Forward, 300_rpm) };
    auto unaffected{ other.move(StepperMotor::Direction::Forward, 300_rpm) };
    ASSERT_TRUE(running(axis));
    ASSERT_TRUE(running(other_axis));
    input->setSimulatedLevel(High); // Press switch / unplug NC cable.
    input->setSimulatedLevel(Low); // A quick release must not erase that activation.
    EXPECT_EQ(result(motion), Stopped);
    EXPECT_EQ(motor.velocity(), 0_rpm);
    const auto stopped{ motor.position() };
    EXPECT_NEAR(stopped.get<pnm::units::AngleUnits::deg>(), *axis->pulseCount() * 0.1125, 1e-9);
    EXPECT_EQ(other_axis->status().state, hal::step::State::Running);
    input->setSimulatedLevel(High);
    auto blocked{ motor.moveRel(1.125_deg, 300_rpm) };
    EXPECT_EQ(result(blocked), Rejected);
    EXPECT_EQ(motor.position(), stopped);
    // Still HIGH: only motion away is allowed. Falling edge must not stop it.
    auto retreat{ motor.moveRel(-11.25_deg, 9.375_rpm) };
    ASSERT_TRUE(running(axis));
    input->setSimulatedLevel(Low);
    input->setSimulatedLevel(High); // Bounce/reactivation while moving away.
    input->setSimulatedLevel(Low);
    EXPECT_EQ(result(retreat), Completed);
    EXPECT_EQ(*axis->pulseCount(), 100U);
    EXPECT_NEAR((motor.position() - stopped).get<pnm::units::AngleUnits::deg>(), -11.25, 1e-9);
    auto next{ motor.moveRel(1.125_deg, 300_rpm) };
    EXPECT_EQ(result(next), Completed); // Released again: a new toward command is allowed.
    other.stopAndWait();
    EXPECT_EQ(result(unaffected), Stopped);
}

TEST(StepperMotor, BriefActivationDuringPrepareIsLatchedAndRejectsBeforeStart)
{
    const auto generator{ std::make_shared<StartHookGenerator>() };
    std::shared_ptr<hal::GpioInput> input;
    generator->prepare_hook = [&input] {
        input->setSimulatedLevel(hal::gpio::Level::High);
        input->setSimulatedLevel(hal::gpio::Level::Low);
    };
    StepperMotor motor{ Motor2, 0_deg, 1.8_deg, 16U, generator };
    input = releasedReference(Motor2);
    ASSERT_TRUE(generator->start());
    auto motion{ motor.move(StepperMotor::Direction::Forward, 300_rpm) };
    EXPECT_EQ(result(motion), Rejected);
    EXPECT_EQ(*generator->generator->output(hal::step::Axis::_2)->pulseCount(), 0U);
    EXPECT_EQ(motor.position(), 0_deg);
}

TEST(StepperMotor, BriefActivationDuringArmingWakesWorkerAndCannotBeLostBeforeWait)
{
    const auto generator{ std::make_shared<StartHookGenerator>() };
    std::shared_ptr<hal::GpioInput> input;
    generator->start_hook = [&input] {
        input->setSimulatedLevel(hal::gpio::Level::High);
        input->setSimulatedLevel(hal::gpio::Level::Low);
    };
    StepperMotor motor{ Motor2, 0_deg, 1.8_deg, 16U, generator };
    input = releasedReference(Motor2);
    ASSERT_TRUE(generator->start());
    auto motion{ motor.move(StepperMotor::Direction::Forward, 300_rpm) };
    EXPECT_EQ(result(motion), Stopped);
    EXPECT_EQ(generator->generator->output(hal::step::Axis::_2)->status().state, hal::step::State::Stopped);
    EXPECT_EQ(motor.velocity(), 0_rpm);
}

TEST(StepperMotor, DestructionUnsubscribesReferenceInputWhileStoppingWorker)
{
    const auto generator{ hal::board::createStepperGenerator() };
    std::shared_ptr<hal::GpioInput> input;
    ASSERT_TRUE(generator->start());
    std::future<StepperMotor::Result> motion;
    {
        StepperMotor motor{ Motor2, 0_deg, 1.8_deg, 16U, generator };
        input = releasedReference(Motor2);
        motion = motor.move(StepperMotor::Direction::Forward, 300_rpm);
        ASSERT_TRUE(running(generator->output(hal::step::Axis::_2)));
    }
    EXPECT_EQ(result(motion), Stopped);
    input->setSimulatedLevel(hal::gpio::Level::High);
    input->setSimulatedLevel(hal::gpio::Level::Low);
    EXPECT_EQ(generator->output(hal::step::Axis::_2)->status().state, hal::step::State::Stopped);
}
