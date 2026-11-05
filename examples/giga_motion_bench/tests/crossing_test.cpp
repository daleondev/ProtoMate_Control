#include "../Crossing.hpp"
#include "../FastCrossing.hpp"
#include <gtest/gtest.h>
using namespace motion_bench;
TEST(MotionBenchCrossing, NonlinearRisingAndFallingIntegerCertificate)
{
    for (bool forward : { false, true }) {
        Crossing s{ { 0, 400, 200, 100, 30, 7 }, 10000000, forward };
        if (!forward) {
            s.c[0] = 737;
            for (unsigned i = 1; i < 6; ++i)
                s.c[i] = -s.c[i];
        }
        double guess = 0;
        for (unsigned i = 0; i < 737; ++i) {
            const double target = forward ? i + .5 : 736.5 - i;
            auto r = s.solve(target, 0, 1, guess);
            ASSERT_LE(r.tick, s.ticks);
            EXPECT_TRUE(s.crossed(r.tick, target));
            if (r.tick)
                EXPECT_FALSE(s.crossed(r.tick - 1, target));
            const double x = double(r.tick) / s.ticks;
            guess = x + (forward ? 1. : -1.) / s.derivative(x);
            EXPECT_LE(r.evaluations, 40U);
        }
    }
}
TEST(MotionBenchCrossing, ZeroDerivativeAndPoorGuessRemainBounded)
{
    Crossing s{ { 0, 0, 0, 0, 0, 1000 }, 10000000 };
    for (auto guess : { 0., .9, 1. }) {
        auto r = s.solve(.5, 0, 1, guess);
        EXPECT_TRUE(s.crossed(r.tick, .5));
        EXPECT_FALSE(s.crossed(r.tick - 1, .5));
        EXPECT_LE(r.evaluations, 40U);
    }
}
TEST(MotionBenchCrossing, TickMatchesOriginalDoubleBisectionWithinOneGridTick)
{
    Crossing s{ { 0, 1900, 400, 300, 200, 100 }, 10000000 };
    for (unsigned i = 0; i < 2900; ++i) {
        const double target = i + .5;
        double lo = 0, hi = 1;
        for (unsigned j = 0; j < 52; ++j) {
            const double mid = (lo + hi) * .5;
            if (s.value(mid) < target)
                lo = mid;
            else
                hi = mid;
        }
        const auto reference = std::uint32_t(std::ceil(hi * s.ticks));
        auto r = s.solve(target, 0, 1, .5);
        EXPECT_LE(std::abs(std::int64_t(r.tick) - reference), 1);
    }
}

TEST(MotionBenchCrossing, OptimizedKernelPreservesEveryCertifiedTick)
{
    for (bool forward : { false, true }) {
        Crossing a{ { 0, 50000, 10000, 5000, 2000, 1000 }, 10000000, forward };
        if (!forward) {
            a.c[0] = 68000;
            for (unsigned i = 1; i < 6; ++i)
                a.c[i] = -a.c[i];
        }
        FastCrossing b{ a.c, a.ticks, forward };
        double guess = 0;
        for (unsigned i = 0; i < 68000; ++i) {
            const auto target = forward ? i + .5 : 67999.5 - i;
            auto reference = a.solve(target, 0, 1, guess);
            auto optimized = b.solve(target, 0, 1, guess);
            ASSERT_EQ(reference.tick, optimized.tick);
            const auto x = double(reference.tick) / a.ticks;
            guess = x + (forward ? 1. : -1.) / a.derivative(x);
        }
    }
}

#include "../BrakeCrossing.hpp"
TEST(MotionBenchCrossing, BrakingProfileIntegerCertificates)
{
    for (double acceleration : { -10000., 0., 1234., 10000. }) {
        const auto profile = motion::Profile::brake(100000, acceleration, { 100000, 1e6, 1e6, 1e7 });
        ASSERT_TRUE(profile);
        motion_bench::BrakeCrossing cursor{ *profile };
        std::uint32_t previous{};
        for (unsigned step = 1; step < profile->distance(); ++step) {
            const auto tick = cursor.next(step);
            EXPECT_GT(tick, previous);
            EXPECT_GE(profile->at(tick * 1e-7).position, step);
            EXPECT_LT(profile->at((tick - 1) * 1e-7).position, step);
            previous = tick;
        }
    }
}
