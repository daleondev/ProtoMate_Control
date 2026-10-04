#include "../AxisConversion.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <type_traits>

namespace
{
    using namespace pnm::units;
    using namespace pnm::units::literals;

    template<IsQuantity Quantity>
    void expectNear(Quantity actual, Quantity expected)
    {
        EXPECT_NEAR(actual.get(), expected.get(), 1e-10);
    }

    static_assert(
      std::is_same_v<decltype(std::declval<LinearAxisConversion>().toAxisPosition(0_deg)), Distance>);
    static_assert(!std::is_invocable_v<decltype(&LinearAxisConversion::toMotorPosition),
                                       const LinearAxisConversion&,
                                       Angle>);
    static_assert(!std::is_invocable_v<decltype(&RotaryAxisConversion::toMotorPosition),
                                       const RotaryAxisConversion&,
                                       Distance>);
}

TEST(AxisConversion, RotaryReductionMapsReferencePositionsWithoutWrapping)
{
    const RotaryAxisConversion axis{ { .motor_revolutions_per_axis_revolution = 5.0,
                                       .motor_reference = 135_deg,
                                       .axis_reference = -30_deg } };

    expectNear(axis.toAxisPosition(135_deg), -30_deg);
    expectNear(axis.toMotorPosition(-30_deg), 135_deg);
    expectNear(axis.toAxisPosition(585_deg), 60_deg);
    expectNear(axis.toMotorPosition(60_deg), 585_deg);
    expectNear(axis.toAxisDisplacement(5_rev), 1_rev);
    expectNear(axis.toMotorDisplacement(-90_deg), -450_deg);
    expectNear(axis.toAxisPosition(135_deg + 20_rev), -30_deg + 4_rev);
    expectNear(axis.toMotorPosition(-30_deg - 4_rev), 135_deg - 20_rev);
}

TEST(AxisConversion, ReversedRotaryFeedbackHasSignButCommandSpeedRemainsPositive)
{
    const RotaryAxisConversion axis{ { .motor_revolutions_per_axis_revolution = 5.0,
                                       .direction = AxisDirection::OppositeToMotor,
                                       .motor_reference = 135_deg,
                                       .axis_reference = 90_deg } };

    expectNear(axis.toAxisPosition(585_deg), 0_deg);
    expectNear(axis.toMotorPosition(0_deg), 585_deg);
    expectNear(axis.toAxisDisplacement(-450_deg), 90_deg);
    expectNear(axis.toMotorDisplacement(90_deg), -450_deg);
    expectNear(axis.toAxisVelocity(300_rpm), -60_rpm);
    expectNear(axis.toMotorVelocity(-60_rpm), 300_rpm);
    expectNear(axis.toAxisVelocity(-300_rpm), 60_rpm);
    expectNear(axis.toMotorVelocity(60_rpm), -300_rpm);
    expectNear(axis.toAxisSpeed(300_rpm), 60_rpm);
    expectNear(axis.toMotorSpeed(60_rpm), 300_rpm);
    expectNear(axis.toAxisVelocity(0_rpm), 0_rpm);
    expectNear(axis.toMotorSpeed(0_rpm), 0_rpm);
}

TEST(AxisConversion, DirectBeltDriveConvertsMillimetresAndRevolutions)
{
    // Illustrative 20-tooth, 2 mm pitch pulley; not a robot configuration.
    const LinearAxisConversion axis{
        { .travel_per_output_revolution = 40_mm, .motor_reference = 135_deg, .axis_reference = 200_mm }
    };

    expectNear(axis.toAxisPosition(135_deg), 200_mm);
    expectNear(axis.toMotorPosition(200_mm), 135_deg);
    expectNear(axis.toAxisPosition(495_deg), 240_mm);
    expectNear(axis.toMotorPosition(240_mm), 495_deg);
    expectNear(axis.toAxisDisplacement(-90_deg), -10_mm);
    expectNear(axis.toMotorDisplacement(-10_mm), -90_deg);
    expectNear(axis.toAxisVelocity(60_rpm), 40_mm_s);
    expectNear(axis.toMotorVelocity(40_mm_s), 60_rpm);
    expectNear(axis.toAxisSpeed(60_rpm), 40_mm_s);
    expectNear(axis.toMotorSpeed(40_mm_s), 60_rpm);
}

TEST(AxisConversion, ReversedLinearDriveIncludesReductionAndReferenceHeight)
{
    const LinearAxisConversion axis{ { .travel_per_output_revolution = 8_mm,
                                       .motor_revolutions_per_output_revolution = 2.0,
                                       .direction = AxisDirection::OppositeToMotor,
                                       .motor_reference = -180_deg,
                                       .axis_reference = 150_mm } };

    expectNear(axis.toAxisPosition(0_deg), 148_mm);
    expectNear(axis.toMotorPosition(148_mm), 0_deg);
    expectNear(axis.toAxisDisplacement(720_deg), -8_mm);
    expectNear(axis.toMotorDisplacement(-8_mm), 720_deg);
    expectNear(axis.toAxisVelocity(120_rpm), -8_mm_s);
    expectNear(axis.toMotorVelocity(-8_mm_s), 120_rpm);
    expectNear(axis.toAxisVelocity(-120_rpm), 8_mm_s);
    expectNear(axis.toMotorVelocity(8_mm_s), -120_rpm);
    expectNear(axis.toAxisSpeed(120_rpm), 8_mm_s);
    expectNear(axis.toMotorSpeed(8_mm_s), 120_rpm);
    expectNear(axis.toAxisVelocity(0_rpm), 0_mm_s);
    expectNear(axis.toMotorSpeed(0_mm_s), 0_rpm);
}

TEST(AxisConversion, FractionalRatiosAndMixedUnitsRoundTrip)
{
    for (const auto direction : { AxisDirection::SameAsMotor, AxisDirection::OppositeToMotor }) {
        const RotaryAxisConversion rotary{ { .motor_revolutions_per_axis_revolution = 0.625,
                                             .direction = direction,
                                             .motor_reference = 0.25_rev,
                                             .axis_reference = -0.5_rad } };
        const LinearAxisConversion linear{ { .travel_per_output_revolution = 0.04_m,
                                             .motor_revolutions_per_output_revolution = 1.5,
                                             .direction = direction,
                                             .motor_reference = 0.25_rev,
                                             .axis_reference = -10_mm } };
        for (const auto position : { -10_rev, -0.125_rad, 0_rad, 270_deg, 10_rev }) {
            expectNear(rotary.toMotorPosition(rotary.toAxisPosition(position)), position);
            expectNear(rotary.toMotorDisplacement(rotary.toAxisDisplacement(position)), position);
            expectNear(linear.toMotorPosition(linear.toAxisPosition(position)), position);
            expectNear(linear.toMotorDisplacement(linear.toAxisDisplacement(position)), position);
        }
    }
}

TEST(AxisConversion, ProfileLimitsScaleWithoutDirectionOrReferenceOffsets)
{
    for (const auto direction : { AxisDirection::SameAsMotor, AxisDirection::OppositeToMotor }) {
        const RotaryAxisConversion rotary{ { .motor_revolutions_per_axis_revolution = 5,
                                             .direction = direction,
                                             .motor_reference = 135_deg,
                                             .axis_reference = 10_deg } };
        const LinearAxisConversion linear{ { .travel_per_output_revolution = 40_mm,
                                             .motor_revolutions_per_output_revolution = 2,
                                             .direction = direction,
                                             .motor_reference = 135_deg,
                                             .axis_reference = 200_mm } };
        expectNear(rotary.toMotorAcceleration(100_deg_s2), 500_deg_s2);
        expectNear(rotary.toAxisAcceleration(500_deg_s2), 100_deg_s2);
        expectNear(rotary.toMotorJerk(1000_deg_s3), 5000_deg_s3);
        expectNear(rotary.toAxisJerk(5000_deg_s3), 1000_deg_s3);
        expectNear(linear.toMotorAcceleration(10_mm_s2), 180_deg_s2);
        expectNear(linear.toAxisAcceleration(180_deg_s2), 10_mm_s2);
        expectNear(linear.toMotorJerk(100_mm_s3), 1800_deg_s3);
        expectNear(linear.toAxisJerk(1800_deg_s3), 100_mm_s3);
        expectNear(rotary.toMotorAcceleration(0_deg_s2), 0_deg_s2);
        expectNear(rotary.toMotorJerk(0_deg_s3), 0_deg_s3);
        expectNear(linear.toMotorAcceleration(0_mm_s2), 0_deg_s2);
        expectNear(linear.toMotorJerk(0_mm_s3), 0_deg_s3);
    }
}

TEST(AxisConversion, InvalidProfileLimitsAreRejectedBeforeTheyCanBecomeDefaults)
{
    const RotaryAxisConversion rotary{ { .motor_revolutions_per_axis_revolution = 5 } };
    const LinearAxisConversion linear{ { .travel_per_output_revolution = 40_mm } };
    for (const auto value :
         { -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() }) {
        EXPECT_THROW(rotary.toMotorAcceleration(AngularAcceleration::create(value)), std::invalid_argument);
        EXPECT_THROW(rotary.toAxisAcceleration(AngularAcceleration::create(value)), std::invalid_argument);
        EXPECT_THROW(rotary.toMotorJerk(AngularJerk::create(value)), std::invalid_argument);
        EXPECT_THROW(rotary.toAxisJerk(AngularJerk::create(value)), std::invalid_argument);
        EXPECT_THROW(linear.toMotorAcceleration(Acceleration::create(value)), std::invalid_argument);
        EXPECT_THROW(linear.toAxisAcceleration(AngularAcceleration::create(value)), std::invalid_argument);
        EXPECT_THROW(linear.toMotorJerk(Jerk::create(value)), std::invalid_argument);
        EXPECT_THROW(linear.toAxisJerk(AngularJerk::create(value)), std::invalid_argument);
    }
    const auto maximum{ std::numeric_limits<double>::max() };
    EXPECT_THROW(rotary.toMotorAcceleration(AngularAcceleration::create(maximum)), std::overflow_error);
    EXPECT_THROW(rotary.toMotorJerk(AngularJerk::create(maximum)), std::overflow_error);
    EXPECT_THROW(linear.toMotorAcceleration(Acceleration::create(maximum)), std::overflow_error);
    EXPECT_THROW(linear.toMotorJerk(Jerk::create(maximum)), std::overflow_error);
}

TEST(AxisConversion, InvalidTransmissionConfigurationIsRejected)
{
    for (const auto ratio :
         { 0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() }) {
        EXPECT_THROW((RotaryAxisConversion{ { .motor_revolutions_per_axis_revolution = ratio } }),
                     std::invalid_argument);
        EXPECT_THROW((LinearAxisConversion{ { .travel_per_output_revolution = 40_mm,
                                              .motor_revolutions_per_output_revolution = ratio } }),
                     std::invalid_argument);
        EXPECT_THROW((LinearAxisConversion{ { .travel_per_output_revolution = Distance::create(ratio) } }),
                     std::invalid_argument);
    }
    EXPECT_THROW((RotaryAxisConversion{ { .motor_revolutions_per_axis_revolution = 1.0,
                                          .direction = static_cast<AxisDirection>(99) } }),
                 std::invalid_argument);
    EXPECT_THROW((LinearAxisConversion{
                   { .travel_per_output_revolution = 40_mm, .direction = static_cast<AxisDirection>(99) } }),
                 std::invalid_argument);

    const auto nan{ std::numeric_limits<double>::quiet_NaN() };
    EXPECT_THROW((RotaryAxisConversion{
                   { .motor_revolutions_per_axis_revolution = 1.0, .motor_reference = Angle::create(nan) } }),
                 std::invalid_argument);
    EXPECT_THROW((RotaryAxisConversion{
                   { .motor_revolutions_per_axis_revolution = 1.0, .axis_reference = Angle::create(nan) } }),
                 std::invalid_argument);
    EXPECT_THROW((LinearAxisConversion{
                   { .travel_per_output_revolution = 40_mm, .motor_reference = Angle::create(nan) } }),
                 std::invalid_argument);
    EXPECT_THROW((LinearAxisConversion{
                   { .travel_per_output_revolution = 40_mm, .axis_reference = Distance::create(nan) } }),
                 std::invalid_argument);
}

TEST(AxisConversion, InvalidInputsAndUnrepresentableResultsAreRejected)
{
    const RotaryAxisConversion rotary{ { .motor_revolutions_per_axis_revolution = 5.0 } };
    const LinearAxisConversion linear{ { .travel_per_output_revolution = 40_mm } };
    for (const auto value : { std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN() }) {
        EXPECT_THROW(rotary.toAxisPosition(Angle::create(value)), std::invalid_argument);
        EXPECT_THROW(rotary.toMotorPosition(Angle::create(value)), std::invalid_argument);
        EXPECT_THROW(rotary.toAxisDisplacement(Angle::create(value)), std::invalid_argument);
        EXPECT_THROW(rotary.toMotorDisplacement(Angle::create(value)), std::invalid_argument);
        EXPECT_THROW(rotary.toAxisVelocity(AngularVelocity::create(value)), std::invalid_argument);
        EXPECT_THROW(rotary.toMotorVelocity(AngularVelocity::create(value)), std::invalid_argument);
        EXPECT_THROW(rotary.toAxisSpeed(AngularVelocity::create(value)), std::invalid_argument);
        EXPECT_THROW(rotary.toMotorSpeed(AngularVelocity::create(value)), std::invalid_argument);
        EXPECT_THROW(linear.toAxisPosition(Angle::create(value)), std::invalid_argument);
        EXPECT_THROW(linear.toMotorPosition(Distance::create(value)), std::invalid_argument);
        EXPECT_THROW(linear.toAxisDisplacement(Angle::create(value)), std::invalid_argument);
        EXPECT_THROW(linear.toMotorDisplacement(Distance::create(value)), std::invalid_argument);
        EXPECT_THROW(linear.toAxisVelocity(AngularVelocity::create(value)), std::invalid_argument);
        EXPECT_THROW(linear.toMotorVelocity(Velocity::create(value)), std::invalid_argument);
        EXPECT_THROW(linear.toAxisSpeed(AngularVelocity::create(value)), std::invalid_argument);
        EXPECT_THROW(linear.toMotorSpeed(Velocity::create(value)), std::invalid_argument);
    }
    EXPECT_THROW(rotary.toAxisSpeed(-1_rpm), std::invalid_argument);
    EXPECT_THROW(rotary.toMotorSpeed(-1_rpm), std::invalid_argument);
    EXPECT_THROW(linear.toAxisSpeed(-1_rpm), std::invalid_argument);
    EXPECT_THROW(linear.toMotorSpeed(-1_mm_s), std::invalid_argument);

    const auto maximum{ std::numeric_limits<double>::max() };
    EXPECT_THROW(rotary.toMotorDisplacement(Angle::create(maximum)), std::overflow_error);
    EXPECT_THROW(linear.toMotorDisplacement(Distance::create(maximum)), std::overflow_error);
    const RotaryAxisConversion offset{ { .motor_revolutions_per_axis_revolution = 1.0,
                                         .motor_reference = Angle::create(-maximum),
                                         .axis_reference = Angle::create(maximum) } };
    EXPECT_THROW(offset.toAxisPosition(Angle::create(maximum)), std::overflow_error);
    EXPECT_THROW(offset.toMotorPosition(Angle::create(-maximum)), std::overflow_error);
    EXPECT_THROW(offset.toAxisPosition(1_rad), std::overflow_error);
    EXPECT_THROW(offset.toMotorPosition(-1_rad), std::overflow_error);
}
