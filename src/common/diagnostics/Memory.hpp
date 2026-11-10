#pragma once

#include <array>
#include <cstdint>

namespace diagnostics
{
    inline constexpr std::size_t THREAD_NAME_BYTES{ 32 }, THREAD_CAPACITY{ 32 };
    enum class MemoryQuery : std::uint8_t
    {
        Heap,
        Stack
    };
    enum class Allocator : std::uint8_t
    {
        Newlib,
        Glibc
    };

    // Value-only snapshots shared by the local CLI and the IO diagnostic service.
    // Fixed bounds keep the complete response below one ICC frame (2048 bytes).
    struct HeapInfo
    {
        std::uint64_t capacity{}; // Zero means a dynamically sized host heap.
        std::uint64_t reserved{};
        std::uint64_t used{};
        std::uint64_t available{};
        std::uint64_t releasable{};
        std::uint64_t free_blocks{};
        std::uint64_t mapped_regions{};
        std::uint64_t mapped_bytes{};
        Allocator allocator{};
    };

    struct ThreadInfo
    {
        std::array<char, THREAD_NAME_BYTES> name{}; // NUL-terminated; overlong names end in '~'.
        std::uint32_t prio{};
        std::uint32_t stack_size{};
        std::uint32_t current_stack_usage{};
        std::uint32_t peak_stack_usage{};
    };

    struct StackInfo
    {
        std::array<ThreadInfo, THREAD_CAPACITY> threads{};
        std::uint32_t count{};
        std::uint32_t total{}; // Report omitted threads explicitly if the bound is exceeded.
    };

    struct MemorySnapshot
    {
        HeapInfo heap{};
        StackInfo stack{};
        MemoryQuery query{};
        bool shared_runtime{}; // Linux has one allocator/kernel for both applications.
    };

    // Thread context only. Collection and transport never format console output.
    [[nodiscard]] MemorySnapshot collect_memory(MemoryQuery query);
    // Periodic recorder sampling: visit one thread at a time, without placing
    // the full IPC response (32 thread records) on the collector's stack.
    void sample_profiling_memory();
}
