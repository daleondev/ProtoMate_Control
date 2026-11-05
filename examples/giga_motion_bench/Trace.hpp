#pragma once
#include <array>
#include <cstdint>
namespace motion_bench
{
    struct TraceEntry
    {
        std::uint32_t time, cycles, stage, lane, size;
        std::array<std::byte, 12> header;
    };
    struct TraceStore
    {
        std::uint32_t count;
        std::array<TraceEntry, 512> entries;
    };
    void beginTrace(bool enabled);
    void endTrace();
    void printTrace(unsigned side);
}
