#include "profiling/Trace.hpp"
#include <cstring>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace
{
    class Recorder : public testing::Test
    {
        void SetUp() override
        {
            profile_initialize();
            profile_freeze(PROFILE_MANUAL);
            profile_allow_arm(1);
            ASSERT_EQ(profile_arm(PROFILE_SYSTEM, 1234), 0);
        }
        void TearDown() override { profile_freeze(PROFILE_MANUAL); }
    };
    TEST_F(Recorder, BoundedRingWrapsAndFreezeIsStable)
    {
        for (unsigned i = 0; i < 5000; ++i)
            profile_event(PROFILE_CAT_SYSTEM, PROFILE_MOTION_STAGE, i, 0, 0, 0);
        EXPECT_GT(profile_status().wraps, 0U);
        profile_freeze(PROFILE_MANUAL);
        const auto before = profile_status();
        const auto sums = protomate_profile_summary;
        profile_event(PROFILE_CAT_SYSTEM, PROFILE_MOTION_STAGE, 0, 0, 0, 0);
        profile_heap(1, 999);
        profile_allocation(1, 32, 123, 456, 0);
        profile_queue(0, 99, 99, 1, 1, 1);
        EXPECT_EQ(profile_status().events, before.events);
        EXPECT_EQ(profile_status().allocation_calls, before.allocation_calls);
        EXPECT_EQ(std::memcmp(&sums, &protomate_profile_summary, sizeof(sums)), 0);
    }
    TEST_F(Recorder, PresetsFilterAndFailurePreservesFirstCause)
    {
        const auto filtered = profile_status().filtered;
        profile_event(PROFILE_CAT_GRAPHICS, PROFILE_GRAPHICS_STAGE, 1, 2, 3, 4);
        EXPECT_EQ(profile_status().filtered, filtered + 1);
        profile_failure(PROFILE_ALLOCATION, 3, 128, 0x1234);
        EXPECT_EQ(profile_status().state, PROFILE_FROZEN);
        EXPECT_EQ(profile_status().failure.caller, 0x1234U);
        profile_failure(PROFILE_FATAL, 9, 999, 0xFFFF);
        EXPECT_EQ(profile_status().failure.reason, PROFILE_ALLOCATION);
        EXPECT_EQ(profile_status().failure.sequence % 2, 0U);
    }
    TEST_F(Recorder, QueueCountersAndLongMeasurementsAreExplicit)
    {
        profile_queue(0, 2, 4, 1, 2, 3);
        profile_queue(0, 1, 4, 2, 3, 4);
        const auto& q = protomate_profile_summary.queues[0];
        EXPECT_EQ(q.current, 1U);
        EXPECT_EQ(q.high_water, 2U);
        EXPECT_EQ(q.blocked, 3U);
        EXPECT_EQ(q.dropped, 5U);
        EXPECT_EQ(q.replaced, 7U);
        profile_measure(0, profile_cycles(), UINT32_MAX, PROFILE_CAT_SYSTEM, 0);
        EXPECT_EQ(protomate_profile_summary.sites[0].invalid, 1U);
    }
    TEST_F(Recorder, ArmingRequiresIdleAndDoesNotOverwriteActiveCapture)
    {
        EXPECT_EQ(profile_arm(PROFILE_SYSTEM, 999), 3);
        EXPECT_EQ(profile_status().capture, 1234U);
        profile_freeze(PROFILE_MANUAL);
        profile_allow_arm(0);
        EXPECT_EQ(profile_arm(PROFILE_SYSTEM, 999), 5);
        EXPECT_EQ(profile_status().capture, 1234U);
        profile_allow_arm(1);
        EXPECT_EQ(profile_arm(PROFILE_MEMORY, 999), 0);
        EXPECT_EQ(profile_status().capture, 999U);
        EXPECT_EQ(profile_status().wraps, 0U);
    }
    TEST_F(Recorder, MeasurementCannotStraddleCaptureGenerations)
    {
        const auto generation = profile_generation();
        profile_freeze(PROFILE_MANUAL);
        ASSERT_EQ(profile_arm(PROFILE_SYSTEM, 2222), 0);
        profile_measure(10, profile_cycles(), 0, PROFILE_CAT_SYSTEM, generation);
        EXPECT_EQ(protomate_profile_summary.sites[10].count, 0U);
    }
    TEST_F(Recorder, ConcurrentThreadWritersAndMailbox)
    {
        std::vector<std::jthread> threads;
        for (unsigned t = 0; t < 4; ++t)
            threads.emplace_back([t] {
                for (unsigned i = 0; i < 100; ++i)
                    profile_event(PROFILE_CAT_SYSTEM, PROFILE_MOTION_STAGE, t, i, 0, 0);
            });
        threads.clear();
        EXPECT_GE(profile_status().events, 400U);
        protomate_profile.operation = 2;
        protomate_profile.request = protomate_profile.acknowledge + 1;
        profile_service();
        EXPECT_EQ(protomate_profile.request, protomate_profile.acknowledge);
        EXPECT_EQ(profile_status().state, PROFILE_FROZEN);
    }
}
