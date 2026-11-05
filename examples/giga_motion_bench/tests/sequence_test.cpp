#include "../FixedSequence.hpp"
#include <gtest/gtest.h>

TEST(FixedSequence, HardwareBatchPeriodsPreserveAbsoluteReferenceCrossings)
{
    for (double speed : {25000.,20000.,12500.}) {
        const auto profile=motion::Profile::create(speed*.4,{speed,speed*10,speed*10,speed*100});
        ASSERT_TRUE(profile);
        const motion_bench::FixedSequence sequence(*profile);
        motion_bench::compact::Curve reference(*profile);
        ASSERT_TRUE(sequence.valid());
        std::uint64_t tick=sequence.firstTick();
        for (unsigned first=0;first<sequence.count();first+=64) {
            std::array<hal::step::Timing,64> batch;
            const auto count=std::min<hal::step::PulseCount>(64,sequence.count()-first);
            ASSERT_TRUE(sequence.generate(first,std::span(batch).first(count))) << speed << " first=" << first;
            for(unsigned i=0;i<count;++i) {
                ASSERT_TRUE(reference.crossed(tick,first+i+1));
                ASSERT_FALSE(reference.crossed(tick-1,first+i+1));
                ASSERT_GE(batch[i].period.count(),40000);
                ASSERT_LE(batch[i].period,sequence.maximumPeriod());
                EXPECT_EQ(batch[i].high_time,std::chrono::microseconds(5));
                tick+=batch[i].period.count()/100;
            }
        }
        std::array<hal::step::Timing,2> excess;
        EXPECT_FALSE(sequence.generate(sequence.count(), excess));
        EXPECT_FALSE(sequence.generate(sequence.count()-1, excess));
    }
}

TEST(FixedSequence, MaximumRateCruiseAndPhaseBoundariesRespectMinimumPeriod)
{
    auto profile=motion::Profile::create(10000,{25000,250000,250000,2500000});
    ASSERT_TRUE(profile);
    motion_bench::FixedCrossing program(*profile);
    std::uint32_t previous{};
    for(unsigned i=1;i<=10000;++i) {
        const auto tick=program.next(i);
        if(i>1) ASSERT_GE(tick-previous,400U) << "ordinal=" << i << " previous=" << previous << " tick=" << tick;
        previous=tick;
    }
}
