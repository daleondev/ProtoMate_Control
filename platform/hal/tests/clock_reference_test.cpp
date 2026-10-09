#include "clock_reference.hpp"

#include <gtest/gtest.h>

namespace
{
    using namespace clock_reference;
    using hal::util::stepAdd;

    TEST(ClockReference, DetectsFastAndSlowClocksFromIndependentTenSecondWindows)
    {
        for (const auto offset : { -640'000, 0, 640'000 }) {
            const Boundary first{ 100'000U, 110'000U };
            const auto elapsed{ static_cast<std::uint32_t>(100'000'000 + offset) };
            const Boundary last{ first.before + elapsed, first.after + elapsed };
            const auto result{ measure(first, last) };
            EXPECT_DOUBLE_EQ(result.hz, 10'000'000.0 + offset / 10.0);
            EXPECT_DOUBLE_EQ(result.error_ppm, offset / 100.0);
            EXPECT_DOUBLE_EQ(result.sampling_bound_ppm, 120.0);
            EXPECT_EQ(result.screen,
                      offset < 0   ? Measurement::Screen::Slow
                      : offset > 0 ? Measurement::Screen::Fast
                                   : Measurement::Screen::Within);
        }
    }

    TEST(ClockReference, MeasurementHandlesActualTimerModulus)
    {
        const Boundary first{ hal::util::step_arr - 1000U, 500U };
        const Boundary last{ stepAdd(first.before, 100'000'000U), stepAdd(first.after, 100'000'000U) };
        const auto result{ measure(first, last) };
        EXPECT_DOUBLE_EQ(result.ticks, 100'000'000.0);
        EXPECT_DOUBLE_EQ(result.error_ppm, 0.0);
        EXPECT_NEAR(result.sampling_bound_ppm, 35.01, 1e-9);
    }

    TEST(ClockReference, UnequalPollingDelaysEncloseTrueFrequency)
    {
        const Boundary first{ 96'000U, 102'000U };         // True boundary at 100000.
        const Boundary last{ 100'735'000U, 100'749'000U }; // True boundary at 100740000.
        const auto result{ measure(first, last) };
        EXPECT_LE(result.error_ppm - result.sampling_bound_ppm, 6400.0);
        EXPECT_GE(result.error_ppm + result.sampling_bound_ppm, 6400.0);
        EXPECT_EQ(result.screen, Measurement::Screen::Fast);
    }

    TEST(ClockReference, ScreenDoesNotOverclaimNearItsThreshold)
    {
        const auto result{ measure({ 0U, 10'000U }, { 100'100'000U, 100'110'000U }) };
        EXPECT_DOUBLE_EQ(result.error_ppm, 1000.0);
        EXPECT_EQ(result.screen, Measurement::Screen::Inconclusive);
    }

    TEST(ClockReference, TracksMinuteRolloverAndTimerRollover)
    {
        Tracker tracker;
        EXPECT_EQ(tracker.observe({ 59U, hal::util::step_arr - 400U, hal::util::step_arr - 300U }),
                  Observation::Waiting);
        EXPECT_EQ(tracker.observe({ 0U, 500U, 600U }), Observation::Boundary);
        EXPECT_EQ(tracker.boundary().before, hal::util::step_arr - 400U);
        EXPECT_EQ(tracker.boundary().after, 600U);
        EXPECT_EQ(tracker.observe({ 0U, 1500U, 1600U }), Observation::Waiting);
    }

    TEST(ClockReference, RejectsSkippedSecondsAndExcessiveAcquisitionLatency)
    {
        Tracker jumped;
        ASSERT_EQ(jumped.observe({ 10U, 0U, 100U }), Observation::Waiting);
        EXPECT_EQ(jumped.observe({ 12U, 10'000U, 10'100U }), Observation::Invalid);
        Tracker delayed;
        ASSERT_EQ(delayed.observe({ 10U, 0U, 100U }), Observation::Waiting);
        EXPECT_EQ(delayed.observe({ 11U, 50'000U, 50'100U }), Observation::Invalid);
        Tracker invalid;
        EXPECT_EQ(invalid.observe({ 60U, 0U, 100U }), Observation::Invalid);
        EXPECT_EQ(invalid.observe({ 0U, 0U, 60'000U }), Observation::Invalid);
        EXPECT_EQ(invalid.observe({ 0U, hal::util::step_park, 10U }), Observation::Invalid);
    }
}
