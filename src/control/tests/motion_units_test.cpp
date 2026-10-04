#include "pneumo/units.hpp"

#include <gtest/gtest.h>

#include <numbers>
#include <type_traits>

using namespace pnm::units;
using namespace pnm::units::literals;

static_assert(std::is_same_v<decltype(1_rad_s / 1_s), AngularAcceleration>);
static_assert(std::is_same_v<decltype(1_rad_s2 / 1_s), AngularJerk>);
static_assert(std::is_same_v<decltype(1_rad_s3 * 1_s), AngularAcceleration>);
static_assert(std::is_same_v<decltype(1_rad_s2 * 1_s), AngularVelocity>);
static_assert(!std::is_convertible_v<Acceleration, AngularAcceleration>);
static_assert(!std::is_convertible_v<AngularAcceleration, AngularJerk>);

TEST(MotionUnits, AngularDynamicsUseRadiansAndSecondsWithDegreeLiterals)
{
    EXPECT_NEAR((180_deg_s2).get<AngularAccelerationUnits::rad_s2>(), std::numbers::pi, 1e-12);
    EXPECT_NEAR((180_deg_s3).get<AngularJerkUnits::rad_s3>(), std::numbers::pi, 1e-12);
    EXPECT_NEAR((90_deg_s2 * 2_s).get<AngularVelocityUnits::rpm>(), 30.0, 1e-12);
    EXPECT_NEAR((360_deg_s3 * 0.25_s).get<AngularAccelerationUnits::deg_s2>(), 90.0, 1e-12);
    EXPECT_NEAR((60_rpm / 2_s).get<AngularAccelerationUnits::deg_s2>(), 180.0, 1e-12);
}
