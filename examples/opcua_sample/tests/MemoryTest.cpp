#include "Memory.h"
#include <gtest/gtest.h>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>

TEST(OpcUaMemory, AlignmentCoalescingAndOverflow) {
    alignas(std::max_align_t) std::array<std::byte, 4096> memory{};
    ASSERT_TRUE(opcua_memory_install(memory.data(), memory.size()));
    auto* first = opcua_memory_allocate(13);
    auto* second = opcua_memory_allocate(32);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(first) % alignof(std::max_align_t), 0);
    EXPECT_FALSE(opcua_memory_install(memory.data(), memory.size()));
    std::memset(first, 0x5a, 13);
    EXPECT_EQ(opcua_memory_resize(first, 8192), nullptr);
    EXPECT_EQ(static_cast<unsigned char*>(first)[12], 0x5a);
    auto* larger = opcua_memory_resize(first, 128);
    ASSERT_NE(larger, nullptr);
    EXPECT_EQ(static_cast<unsigned char*>(larger)[12], 0x5a);
    opcua_memory_release(second);
    opcua_memory_release(larger);
    EXPECT_EQ(opcua_memory_stats().used, 0);
    auto* whole = opcua_memory_allocate(3900);
    ASSERT_NE(whole, nullptr);
    opcua_memory_release(whole);
    EXPECT_EQ(opcua_memory_callocate(std::numeric_limits<std::size_t>::max(), 2), nullptr);
    EXPECT_EQ(opcua_memory_allocate(std::numeric_limits<std::size_t>::max()), nullptr);
    EXPECT_EQ(opcua_memory_stats().failures, 3);
}

TEST(OpcUaMemory, ZeroingAndBoundedRepeatedLifetime) {
    alignas(std::max_align_t) std::array<std::byte, 8192> memory{};
    ASSERT_TRUE(opcua_memory_install(memory.data(), memory.size()));
    for(int repeat = 0; repeat < 100; ++repeat) {
        std::array<void*, 32> allocations{};
        for(std::size_t i = 0; i < allocations.size(); ++i) {
            allocations[i] = opcua_memory_callocate(1, i + 1);
            ASSERT_NE(allocations[i], nullptr);
            EXPECT_EQ(static_cast<unsigned char*>(allocations[i])[i], 0);
        }
        for(std::size_t i = 0; i < allocations.size(); i += 2) opcua_memory_release(allocations[i]);
        for(std::size_t i = 1; i < allocations.size(); i += 2) opcua_memory_release(allocations[i]);
        EXPECT_EQ(opcua_memory_stats().used, 0);
    }
    EXPECT_EQ(opcua_memory_stats().failures, 0);
}

TEST(OpcUaMemory, DisjointRegionsNeverCoalesceAcrossTheGap) {
    alignas(std::max_align_t) std::array<std::byte, 8192> memory{};
    ASSERT_TRUE(opcua_memory_install(memory.data(), 2048));
    EXPECT_FALSE(opcua_memory_add_region(memory.data() + 16, 1024));
    ASSERT_TRUE(opcua_memory_add_region(memory.data() + 4096, 2048));
    EXPECT_EQ(opcua_memory_stats().capacity, 4096);
    auto* a = opcua_memory_allocate(1800);
    auto* b = opcua_memory_allocate(1800);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_FALSE(opcua_memory_add_region(memory.data() + 2048, 2048));
    opcua_memory_release(a);
    opcua_memory_release(b);
    EXPECT_EQ(opcua_memory_stats().used, 0);
    EXPECT_EQ(opcua_memory_allocate(3000), nullptr);
    a = opcua_memory_allocate(1800);
    b = opcua_memory_allocate(1800);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    opcua_memory_release(a);
    opcua_memory_release(b);
}
