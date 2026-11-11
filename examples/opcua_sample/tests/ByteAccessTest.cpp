#include <gtest/gtest.h>
#include "ByteAccess.h"
#include <array>
#include <cstring>

TEST(SdramByteAccess, CopiesEveryRelativeAlignmentAndCrossesCacheLines)
{
    std::array<unsigned char, 640> source{}, actual{}, expected{};
    for (unsigned i{}; i < source.size(); ++i) source[i] = static_cast<unsigned char>(i * 37 + 11);
    for (unsigned from{}; from < 32; ++from) {
        for (unsigned to{}; to < 32; ++to) {
            actual.fill(0xCC); expected.fill(0xCC);
            std::memcpy(expected.data() + to, source.data() + from, 257);
            network_memcpy(actual.data() + to, source.data() + from, 257);
            EXPECT_EQ(actual, expected);
            EXPECT_EQ(network_memcmp(actual.data() + to, source.data() + from, 257), 0);
        }
    }
}
TEST(SdramByteAccess, OverlappingMovesAndZeroLength)
{
    std::array<unsigned char, 640> expected{}, actual{};
    for (unsigned from{}; from < 32; ++from) {
        for (unsigned to{}; to < 32; ++to) {
            for (unsigned i{}; i < actual.size(); ++i) actual[i] = expected[i] = static_cast<unsigned char>(i);
            std::memmove(expected.data() + to, expected.data() + from, 257);
            network_memmove(actual.data() + to, actual.data() + from, 257);
            EXPECT_EQ(actual, expected);
        }
    }
    EXPECT_EQ(network_memcpy(nullptr, nullptr, 0), nullptr);
    EXPECT_EQ(network_memcmp(nullptr, nullptr, 0), 0);
}
TEST(SdramByteAccess, StringsAndBoundedPadding)
{
    std::array<char, 128> buffer{};
    for (unsigned offset{}; offset < 32; ++offset) {
        auto* value{ buffer.data() + offset };
        network_strcpy(value, "ProtoMate SDRAM access check");
        EXPECT_EQ(network_strlen(value), std::strlen("ProtoMate SDRAM access check"));
        EXPECT_EQ(network_strcmp(value, "ProtoMate SDRAM access check"), 0);
        EXPECT_EQ(network_strstr(value, "SDRAM"), value + 10);
        EXPECT_EQ(network_strchr(value, 't'), value + 3);
        EXPECT_EQ(network_strrchr(value, 'c'), std::strrchr(value, 'c'));
        EXPECT_EQ(network_strchr(value, 0), value + std::strlen(value));
        network_strncpy(value, "ab", 8);
        EXPECT_EQ(network_strlen(value), 2U);
        for (unsigned i{ 2 }; i < 8; ++i) EXPECT_EQ(value[i], 0);
    }
}
