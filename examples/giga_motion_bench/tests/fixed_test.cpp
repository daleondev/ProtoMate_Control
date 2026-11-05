#include "../FixedCrossing.hpp"
#include <gtest/gtest.h>

TEST(FixedCrossing, CertifiesEveryStoppingPulseAgainstDoubleReference)
{
    for (double speed : {100.,1000.,10000.,90000.,95000.,100000.})
        for (double acceleration : {-10000.,0.,1234.}) {
            auto profile = motion::Profile::brake(speed, acceleration, {100001,1e6,1e6,1e7});
            ASSERT_TRUE(profile);
            motion_bench::FixedCrossing fixed(*profile);
            motion_bench::compact::Curve reference(*profile);
            ASSERT_TRUE(fixed.valid());
            for (unsigned i = 1; i <= reference.count(); ++i) {
                const auto tick = fixed.next(i);
                ASSERT_TRUE(reference.crossed(tick, i)) << speed << ' ' << acceleration << ' ' << i;
                ASSERT_FALSE(reference.crossed(tick-1, i)) << speed << ' ' << acceleration << ' ' << i;
            }
        }
}
TEST(FixedCrossing, AccelerationPhasesAndRestBoundaries)
{
    for (double distance : {1.,10.,100.,1000.,10000.}) {
        auto profile = motion::Profile::create(distance, {100000,1e6,1e6,1e7});
        ASSERT_TRUE(profile);
        motion_bench::FixedCrossing fixed(*profile);
        motion_bench::compact::Curve reference(*profile);
        ASSERT_TRUE(fixed.valid());
        for (unsigned i = 1; i <= reference.count(); ++i) {
            const auto tick = fixed.next(i);
            ASSERT_TRUE(reference.crossed(tick, i)) << distance << ' ' << i;
            ASSERT_FALSE(reference.crossed(tick-1, i)) << distance << ' ' << i;
        }
    }
}

TEST(FixedCrossing, DeterministicVariedStatesKeepTheReferenceCrossing)
{
    std::uint32_t random = 0x7e4c192b;
    const auto next = [&] { random = random * 1664525U + 1013904223U; return random; };
    for (unsigned fixture = 0; fixture < 100; ++fixture) {
        const auto speed = 100. + next() % 99900;
        const auto acceleration = double(int(next() % 20001) - 10000);
        auto profile = motion::Profile::brake(speed, acceleration, {100001,1e6,1e6,1e7});
        ASSERT_TRUE(profile);
        motion_bench::FixedCrossing fixed(*profile);
        motion_bench::compact::Curve reference(*profile);
        ASSERT_TRUE(fixed.valid());
        for (unsigned ordinal = 1; ordinal <= reference.count(); ++ordinal) {
            const auto tick = fixed.next(ordinal);
            ASSERT_TRUE(reference.crossed(tick, ordinal)) << fixture << ' ' << ordinal;
            ASSERT_FALSE(reference.crossed(tick-1, ordinal)) << fixture << ' ' << ordinal;
        }
        EXPECT_EQ(fixed.next(reference.count()+1), 0U);
        EXPECT_FALSE(fixed.valid());
    }
}

TEST(FixedCrossing, IndependentCursorsCanStartAtArbitraryDmaBoundaries)
{
    auto profile = motion::Profile::create(10000, {25000,250000,250000,2500000});
    ASSERT_TRUE(profile);
    const motion_bench::FixedCrossing program(*profile);
    motion_bench::compact::Curve reference(*profile);
    for (unsigned first = 1; first <= reference.count(); first += 61) {
        motion_bench::FixedCrossing::Cursor cursor;
        for (unsigned i = first; i < std::min(first + 64, reference.count() + 1); ++i) {
            auto tick = program.next(i, cursor);
            ASSERT_TRUE(reference.crossed(tick, i)) << i;
            ASSERT_FALSE(reference.crossed(tick-1, i)) << i;
        }
    }
}

TEST(FixedCrossing, GridAlignedCruiseUsesExactIntegerComparison)
{
    for(double speed : {10000.,12500.,20000.,25000.}) {
        auto profile=motion::Profile::create(10000,{speed,speed*10,speed*10,speed*100},speed,0,speed);
        ASSERT_TRUE(profile);
        motion_bench::FixedCrossing program(*profile);
        for(unsigned i=1;i<=10000;++i) EXPECT_EQ(program.next(i),i*unsigned(1e7/speed));
        EXPECT_EQ(program.fallbacks,0U);
    }
}
