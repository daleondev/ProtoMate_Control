#include "../Metrics.hpp"
#include <gtest/gtest.h>
#include <random>
using namespace motion_bench;
TEST(MotionBenchHistogram, ConservativeBoundsAcrossEntireUint32Range)
{
    std::mt19937 random{ 0x12345678 };
    for (unsigned i = 0; i < 100000; ++i) {
        const auto value = std::uint32_t(random());
        EXPECT_GE(Histogram::upper(Histogram::index(value)), value);
    }
    for (unsigned bit = 0; bit < 32; ++bit)
        for (auto value : { std::uint32_t(1U << bit), std::uint32_t((1U << bit) - 1) })
            EXPECT_GE(Histogram::upper(Histogram::index(value)), value);
}
TEST(MotionBenchHistogram, PercentilesAndExactMaximum)
{
    Histogram h;
    EXPECT_EQ(h.percentile(999), 0U);
    for (unsigned i = 0; i < 1000; ++i)
        h.add(i);
    EXPECT_EQ(h.count, 1000U);
    EXPECT_EQ(h.sum, 499500U);
    EXPECT_GE(h.percentile(500), 499U);
    EXPECT_LE(h.percentile(500), 550U);
    EXPECT_GE(h.percentile(950), 949U);
    EXPECT_EQ(h.percentile(1000), 999U);
}
TEST(MotionBenchFrame, ByteOrderBoundsAndCorruption)
{
    std::array<std::byte, 2048> bytes{};
    for (auto size : { 16U, 32U, 128U, 512U, 1536U, 2048U }) {
        auto frame = std::span(bytes).first(size);
        packet(frame, 0x87654321, 0xFFFFFFF0, 0x20);
        EXPECT_EQ(bytes[0], std::byte{ 0x21 });
        EXPECT_EQ(bytes[3], std::byte{ 0x87 });
        EXPECT_EQ(read32(frame, 4), 0xFFFFFFF0U);
        EXPECT_TRUE(valid(frame));
        for (unsigned byte = 0; byte < size; ++byte) {
            frame[byte] ^= std::byte{ 1 };
            EXPECT_FALSE(valid(frame));
            frame[byte] ^= std::byte{ 1 };
        }
    }
    for (unsigned size = 0; size < 16; ++size)
        EXPECT_FALSE(valid(std::span(bytes).first(size)));
}
TEST(MotionBenchClock, UnsignedDeltaAcrossRollover)
{
    const std::uint32_t scheduled = 0xFFFFFFF0, received = 0x20;
    Histogram h;
    h.add(received - scheduled);
    EXPECT_EQ(h.maximum, 48U);
}
