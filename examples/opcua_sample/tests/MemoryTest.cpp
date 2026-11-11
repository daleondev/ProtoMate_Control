#include "ui/opcua/util/Memory.h"
#include <gtest/gtest.h>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <random>
#include <open62541/server.h>
#include <open62541/server_config_default.h>

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

TEST(OpcUaMemory, NeighbourTagsSurviveSplitMergeAndAdjacentRegionBoundaries) {
    alignas(std::max_align_t) std::array<std::byte, 8192> memory{};
    ASSERT_TRUE(opcua_memory_install(memory.data(), 4096));
    ASSERT_TRUE(opcua_memory_add_region(memory.data()+4096, 4096));
    std::array<void*, 12> blocks{};
    for(auto& block : blocks) {block=opcua_memory_allocate(300);ASSERT_NE(block,nullptr);}
    opcua_memory_release(blocks[4]); blocks[4]=nullptr;
    auto* split=opcua_memory_allocate(100); ASSERT_NE(split,nullptr);
    opcua_memory_release(blocks[5]); blocks[5]=nullptr;
    opcua_memory_release(split);
    opcua_memory_release(blocks[3]); blocks[3]=nullptr;
    for(auto* block:blocks) opcua_memory_release(block);
    EXPECT_EQ(opcua_memory_stats().used,0);
    EXPECT_EQ(opcua_memory_allocate(5000),nullptr); // Even touching regions stay independent.
    auto* first=opcua_memory_allocate(3900); auto* second=opcua_memory_allocate(3900);
    ASSERT_NE(first,nullptr); ASSERT_NE(second,nullptr);
    opcua_memory_release(first); opcua_memory_release(second);
    EXPECT_EQ(opcua_memory_stats().used,0);
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

TEST(OpcUaMemory, FragmentedReusePreservesLiveAllocationsAndCoalescesBothNeighbours) {
    alignas(std::max_align_t) std::array<std::byte, 32768> memory{};
    ASSERT_TRUE(opcua_memory_install(memory.data(), memory.size()));
    struct Allocation { unsigned char* data{}; std::size_t size{}; unsigned char tag{}; };
    std::array<Allocation, 96> live{};
    std::mt19937 random{0x62541};
    for(unsigned turn=0; turn<20000; ++turn) {
        auto& a=live[random()%live.size()];
        if(a.data) {
            for(std::size_t i=0; i<a.size; ++i) ASSERT_EQ(a.data[i],a.tag);
            opcua_memory_release(a.data); a={};
        } else {
            a.size=1+random()%256;
            a.data=static_cast<unsigned char*>(opcua_memory_allocate(a.size));
            ASSERT_NE(a.data,nullptr);
            a.tag=static_cast<unsigned char>(random());
            std::memset(a.data,a.tag,a.size);
        }
    }
    for(auto& a:live) if(a.data) {
        for(std::size_t i=0; i<a.size; ++i) ASSERT_EQ(a.data[i],a.tag);
        opcua_memory_release(a.data);
    }
    EXPECT_EQ(opcua_memory_stats().used,0);
    EXPECT_EQ(opcua_memory_stats().failures,0);
    auto* whole=opcua_memory_allocate(memory.size()-128);
    ASSERT_NE(whole,nullptr);
    opcua_memory_release(whole);
}

TEST(OpcUaMemory, StringMethodArgumentIdsAreReleasedWithServer) {
    alignas(std::max_align_t) static std::array<std::byte, 1024*1024> memory{};
    for(unsigned repetition=0; repetition<3; ++repetition) {
        ASSERT_TRUE(opcua_memory_install(memory.data(),memory.size()));
        auto* server=UA_Server_new();
        ASSERT_NE(server,nullptr);
        UA_Argument argument{};
        argument.name=UA_STRING(const_cast<char*>("value"));
        argument.dataType=UA_TYPES[UA_TYPES_DOUBLE].typeId;
        argument.valueRank=UA_VALUERANK_SCALAR;
        auto attributes=UA_MethodAttributes_default;
        attributes.executable=true; attributes.userExecutable=true;
        const auto ns=UA_Server_addNamespace(server,"urn:test:owned-method-ids");
        UA_NodeId input{}, output{};
        EXPECT_EQ(UA_Server_addMethodNodeEx(server,UA_NODEID_STRING(ns,const_cast<char*>("method")),
            UA_NODEID_NUMERIC(0,UA_NS0ID_OBJECTSFOLDER),UA_NODEID_NUMERIC(0,UA_NS0ID_HASCOMPONENT),
            UA_QUALIFIEDNAME(ns,const_cast<char*>("method")),attributes,nullptr,
            1,&argument,UA_NODEID_STRING(ns,const_cast<char*>("method/InputArguments")),&input,
            1,&argument,UA_NODEID_STRING(ns,const_cast<char*>("method/OutputArguments")),&output,
            nullptr,nullptr),UA_STATUSCODE_GOOD);
        EXPECT_EQ(input.identifierType,UA_NODEIDTYPE_STRING);
        EXPECT_EQ(output.identifierType,UA_NODEIDTYPE_STRING);
        UA_NodeId_clear(&input); UA_NodeId_clear(&output);
        UA_Server_delete(server);
        EXPECT_EQ(opcua_memory_stats().used,0);
    }
}

TEST(OpcUaMemory, SmallObjectsDoNotConsumeTheOnlyNetworkSizedHole) {
    alignas(std::max_align_t) std::array<std::byte,32768> memory{};
    ASSERT_TRUE(opcua_memory_install(memory.data(),memory.size()));
    auto* small=opcua_memory_allocate(2048);
    auto* guard1=opcua_memory_allocate(32);
    auto* network=opcua_memory_allocate(8192);
    auto* guard2=opcua_memory_allocate(32);
    ASSERT_NE(small,nullptr); ASSERT_NE(network,nullptr);
    ASSERT_NE(guard1,nullptr); ASSERT_NE(guard2,nullptr);
    std::array<void*,64> rest{};
    for(auto& p:rest) {p=opcua_memory_allocate(512);if(!p)break;}
    opcua_memory_release(small);
    opcua_memory_release(network); // Most recently freed is the large hole.
    auto* replacement=opcua_memory_allocate(2048);
    ASSERT_NE(replacement,nullptr);
    network=opcua_memory_allocate(8192);
    ASSERT_NE(network,nullptr);
    opcua_memory_release(replacement);opcua_memory_release(network);
    opcua_memory_release(guard1);opcua_memory_release(guard2);
    for(auto* p:rest) opcua_memory_release(p);
    EXPECT_EQ(opcua_memory_stats().used,0);
}
