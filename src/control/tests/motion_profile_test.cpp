// Numeric values describe explicit test inputs, timing bounds and expected results.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
#include "../MotionProfile.hpp"
#include "../MotionSequence.hpp"
#include "pneumo/units.hpp"
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <random>

namespace
{
    void verify(const motion::Profile& p, motion::Profile::Limits limits, double ceiling)
    {
        const auto dt{ p.duration() / 2000.0 };
        auto previous{ p.at(0.0) };
        for (unsigned i = 1; i <= 2000; ++i) {
            const auto t{ dt * i };
            const auto current{ p.at(t) };
            ASSERT_GE(current.position + 1e-10, previous.position);
            ASSERT_GE(current.velocity, -1e-9);
            ASSERT_LE(current.velocity, ceiling + 1e-8);
            ASSERT_LE(current.acceleration, limits.acceleration + 1e-8);
            ASSERT_GE(current.acceleration, -limits.deceleration - 1e-8);
            if (limits.jerk > 0.0)
                ASSERT_LE(std::abs(current.acceleration - previous.acceleration), (limits.jerk * dt) + 1e-8);
            // Inversion must recover the same continuous trajectory.
            ASSERT_NEAR(p.timeAt(current.position), t, std::max(1e-8, p.duration() * 1e-7));
            previous = current;
        }
        EXPECT_DOUBLE_EQ(p.at(p.duration()).position, p.distance());
        EXPECT_DOUBLE_EQ(p.at(p.duration()).acceleration, 0.0);
    }
}

TEST(MotionProfile, TriangularTrapezoidalAndJerkLimitedProfilesObeyLimits)
{
    for (const double distance : { 0.0001, 0.1, 1.0, 100.0 }) {
        for (const double jerk : { 0.0, 0.1, 10.0, 10000.0 }) {
            const motion::Profile::Limits limits{
                .velocity = 4.0, .acceleration = 2.0, .deceleration = 5.0, .jerk = jerk
            };
            const auto p{ motion::Profile::create(distance, limits) };
            if (!p) {
                FAIL() << "expected p to have a value" << distance << ", " << jerk;
            }
            EXPECT_DOUBLE_EQ(p->at(0).velocity, 0.0);
            EXPECT_DOUBLE_EQ(p->endVelocity(), 0.0);
            verify(*p, limits, limits.velocity);
        }
    }
    auto trapezoid{ motion::Profile::create(
      20.0, { .velocity = 4.0, .acceleration = 2.0, .deceleration = 2.0, .jerk = 0.0 }) };
    if (!trapezoid) {
        FAIL() << "expected trapezoid to have a value";
    }
    EXPECT_NEAR(trapezoid->duration(), 7.0, 1e-12); // 2 s up, 3 s cruise, 2 s down.
    auto triangle{ motion::Profile::create(
      2.0, { .velocity = 4.0, .acceleration = 2.0, .deceleration = 2.0, .jerk = 0.0 }) };
    if (!triangle) {
        FAIL() << "expected triangle to have a value";
    }
    EXPECT_NEAR(triangle->duration(), 2.0, 1e-12);
}

TEST(MotionProfile, LiveSplicesPreserveVelocityAndAcceleration)
{
    const motion::Profile::Limits limits{
        .velocity = 4.0, .acceleration = 2.0, .deceleration = 3.0, .jerk = 10.0
    };
    auto original{ motion::Profile::create(50.0, limits) };
    if (!original) {
        FAIL() << "expected original to have a value";
    }
    for (const double time : { 0.05, 0.3, 1.95, 2.15, 5.0, 12.8 }) {
        const auto state{ original->at(time) };
        auto changed_limits{ limits };
        changed_limits.velocity = 1.0;
        const auto p{ motion::Profile::create(40.0, changed_limits, state.velocity, state.acceleration) };
        if (!p) {
            FAIL() << "expected p to have a value" << time;
        }
        EXPECT_NEAR(p->at(0).velocity, state.velocity, 1e-12);
        EXPECT_NEAR(p->at(0).acceleration, state.acceleration, 1e-12);
        // Existing positive acceleration takes finite time to remove at bounded jerk.
        const auto ceiling{ std::max(1.0,
                                     state.velocity + std::max(0.0, state.acceleration) *
                                                        std::max(0.0, state.acceleration) /
                                                        (2.0 * limits.jerk)) };
        verify(*p, changed_limits, ceiling);
    }
}

TEST(MotionProfile, BlendEndpointsAndInfeasibleStoppingDistances)
{
    const motion::Profile::Limits limits{
        .velocity = 4.0, .acceleration = 2.0, .deceleration = 3.0, .jerk = 10.0
    };
    for (const double start : { 0.0, 1.0, 4.0, 6.0 }) {
        for (const double end : { 0.0, 1.0, 4.0, 6.0 }) {
            auto p{ motion::Profile::create(30.0, limits, start, 0.0, end) };
            if (!p) {
                FAIL() << "expected p to have a value";
            }
            EXPECT_NEAR(p->at(0).velocity, start, 1e-12);
            EXPECT_DOUBLE_EQ(p->endVelocity(), end);
            verify(*p, limits, std::max({ limits.velocity, start, end }));
        }
    }
    EXPECT_FALSE(motion::Profile::create(0.01, limits, 4.0));
    EXPECT_FALSE(motion::Profile::create(0.01, limits, 0.0, 0.0, 4.0));
    EXPECT_FALSE(motion::Profile::create(10.0, limits, 0.0, -2.0));
    EXPECT_FALSE(motion::Profile::create(10.0, limits, 2.0, 3.0));
}

TEST(MotionProfile, PulseTimesAndBoundsRemainValidAcrossRandomProfiles)
{
    std::mt19937 rng{ 42 };
    std::uniform_real_distribution<double> magnitude{ -2.0, 2.0 };
    for (unsigned trial = 0; trial < 100; ++trial) {
        const double step{ 0.0019634954084936209 };
        const auto count{ 1U + (rng() % 4000U) };
        const motion::Profile::Limits limits{ .velocity = std::pow(10.0, magnitude(rng)),
                                              .acceleration = std::pow(10.0, magnitude(rng)),
                                              .deceleration = std::pow(10.0, magnitude(rng)),
                                              .jerk = trial % 2 ? std::pow(10.0, magnitude(rng)) : 0.0 };
        const auto p{ motion::Profile::create(static_cast<double>(count) * step, limits) };
        if (!p) {
            FAIL() << "expected p to have a value";
        }
        motion::Sequence sequence{ step, { .profile = *p, .begin = 0U, .end = count } };
        std::vector<hal::step::Timing> block(count);
        ASSERT_TRUE(sequence.generate(0U, block));
        double previous{};
        for (unsigned i = 1; i <= count; ++i) {
            const auto time{ sequence.timeAtPulse(i) };
            ASSERT_GT(time, previous);
            EXPECT_NEAR(p->at(time).position, i * step, 1e-8);
            const auto timing{ sequence.timing(i - 1U) };
            ASSERT_TRUE(timing);
            ASSERT_LE(timing->period, sequence.maximumPeriod());
            ASSERT_LE(std::abs((timing->period - block[i - 1U].period).count()), 1);
            ASSERT_LE(block[i - 1U].period, sequence.maximumPeriod());
            previous = time;
        }
    }
}

TEST(MotionProfile, InvalidAndUnrepresentableInputsAreRejected)
{
    const auto nan{ std::numeric_limits<double>::quiet_NaN() };
    EXPECT_FALSE(motion::Profile::create(0.0, { 1, 1, 1, 1 }));
    EXPECT_FALSE(motion::Profile::create(-1.0, { 1, 1, 1, 1 }));
    EXPECT_FALSE(motion::Profile::create(nan, { 1, 1, 1, 1 }));
    for (auto limits :
         { motion::Profile::Limits{ .velocity = 0, .acceleration = 1, .deceleration = 1, .jerk = 1 },
           { .velocity = 1, .acceleration = 0, .deceleration = 1, .jerk = 1 },
           { .velocity = 1, .acceleration = 1, .deceleration = -1, .jerk = 1 },
           { .velocity = 1, .acceleration = 1, .deceleration = 1, .jerk = -1 },
           { .velocity = nan, .acceleration = 1, .deceleration = 1, .jerk = 1 } })
        EXPECT_FALSE(motion::Profile::create(1.0, limits));
}

TEST(MotionProfile, ShortSuccessorCanDecelerateDirectlyFromAHigherBlendSpeed)
{
    // 120 rpm entering a 60 rpm, half-revolution successor. Flattening at
    // 60 rpm before stopping would need too much distance, but a continuous
    // jerk-limited deceleration fits and must retain the requested entry speed.
    using namespace pnm::units::literals;
    const motion::Profile::Limits limits{ .velocity = (60_rpm).get(),
                                          .acceleration = (1440_deg_s2).get(),
                                          .deceleration = (2160_deg_s2).get(),
                                          .jerk = (14400_deg_s3).get() };
    const auto profile{ motion::Profile::create((180_deg).get(), limits, (120_rpm).get()) };
    if (!profile) {
        FAIL() << "expected profile to have a value";
    }
    EXPECT_DOUBLE_EQ(profile->at(0).velocity, (120_rpm).get());
    verify(*profile, limits, (120_rpm).get());
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
