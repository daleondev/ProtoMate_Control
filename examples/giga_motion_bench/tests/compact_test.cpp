#include "../CompactRuns.hpp"
#include <gtest/gtest.h>
#include <vector>
using namespace motion_bench::compact;

TEST(CompactRuns, BrakingPreservesEveryIntegerCrossing)
{
    for (double velocity : { 100., 1000., 10000., 90000., 95000., 100000. }) {
        for (double acceleration : { -10000., 0., 1234. }) {
            auto profile = motion::Profile::brake(velocity, acceleration, { 100001, 1e6, 1e6, 1e7 });
            ASSERT_TRUE(profile);
            Curve curve(*profile);
            Program program;
            ASSERT_TRUE(compile(curve, program)) << velocity << " " << acceleration << " " << program.pulses;
            EXPECT_LE(program.size, CAPACITY);
            std::vector<std::uint32_t> ticks(program.pulses);
            ASSERT_TRUE(expand(program, 0, ticks));
            for (unsigned i = 0; i < ticks.size(); ++i) {
                EXPECT_TRUE(curve.crossed(ticks[i], i + 1.));
                EXPECT_FALSE(curve.crossed(ticks[i] - 1, i + 1.));
                if (i) EXPECT_GT(ticks[i], ticks[i - 1]);
                EXPECT_NEAR(ticks[i], profile->timeAt(i + 1.) * 1e7, 1.01);
            }
            for (unsigned first = 0; first < ticks.size(); first += 137) {
                std::array<std::uint32_t, 137> batch{};
                const auto count = std::min<unsigned>(batch.size(), ticks.size() - first);
                ASSERT_TRUE(expand(program, first, std::span(batch).first(count)));
                EXPECT_TRUE(std::equal(batch.begin(), batch.begin() + count, ticks.begin() + first));
            }
        }
    }
}

TEST(CompactRuns, AccelerationAndZeroVelocityBoundaries)
{
    for (double distance : { 1., 10., 100., 1000., 10000. }) {
        auto profile = motion::Profile::create(distance, { 100000, 1e6, 1e6, 1e7 });
        ASSERT_TRUE(profile);
        Curve curve(*profile);
        Program program;
        ASSERT_TRUE(compile(curve, program)) << distance;
        for (unsigned r = 0; r < program.size; ++r) {
            ASSERT_TRUE(valid(program.runs[r]));
            for (unsigned i = 0; i < program.runs[r].count; ++i) {
                auto t = tick(program.runs[r], i);
                auto target = program.runs[r].first + i + 1.;
                EXPECT_TRUE(curve.crossed(t, target));
                EXPECT_FALSE(curve.crossed(t - 1, target));
            }
        }
    }
}

TEST(CompactRuns, ExactMaximumRateCruiseHasNoNinetyNineTickIntervals)
{
    const auto profile = motion::Profile::create(10000, {100000, 1e6, 1e6, 1e7}, 100000, 0, 100000);
    ASSERT_TRUE(profile);
    Program program;
    ASSERT_TRUE(compile(Curve(*profile), program));
    for (unsigned r = 0; r < program.size; ++r)
        for (unsigned i = 0; i < program.runs[r].count; ++i)
            EXPECT_EQ(tick(program.runs[r], i), (program.runs[r].first + i + 1) * 100);
}

TEST(CompactRuns, MalformedRunsAreRejectedBeforeExpansion)
{
    motion_bench::compact::Run run{ .count = 2, .start = std::int64_t{100} << 32, .interval = std::int64_t{100} << 32 };
    ASSERT_TRUE(valid(run));
    run.plus[0] = run.minus[0] = 1;
    EXPECT_FALSE(valid(run));
    run = { .count = 129 };
    EXPECT_FALSE(valid(run));
    run = { .count = 128, .start = MAX_START, .interval = MAX_INTERVAL, .add = MAX_ADD };
    EXPECT_FALSE(valid(run));
    run = { .count = 128, .start = 0, .interval = 0, .add = -MAX_ADD };
    EXPECT_FALSE(valid(run));
}
