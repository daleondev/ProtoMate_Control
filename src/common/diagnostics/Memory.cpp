#include "Memory.hpp"

#include <algorithm>
#include <cstddef>
#include <malloc.h>
#include <stdexcept>
#include <tx_api.h>
#include <tx_thread.h>

#if defined(PNM_PLATFORM_LINUX)
#include "tx_thread_stack_info.hpp"
#else
extern "C" {
extern std::byte __HeapBase;
extern std::byte __HeapLimit;
}
#endif

namespace diagnostics
{
    namespace
    {
        HeapInfo collect_heap()
        {
            HeapInfo result{};
#if defined(PNM_PLATFORM_LINUX)
            const auto info{ ::mallinfo2() };
            result.allocator = Allocator::Glibc;
            result.reserved = info.arena + info.hblkhd;
            result.used = info.uordblks + info.hblkhd;
            result.available = info.fordblks;
#else
            const auto info{ ::mallinfo() };
            // These linker symbols denote addresses, not an array object.
            const auto base{ reinterpret_cast<std::uintptr_t>(&__HeapBase) };
            const auto limit{ reinterpret_cast<std::uintptr_t>(&__HeapLimit) };
            result.capacity = limit - base;
            result.reserved = info.arena;
            result.used = info.uordblks;
            result.available = info.fordblks +
              (result.reserved < result.capacity ? result.capacity - result.reserved : 0U);
#endif
            result.releasable = info.keepcost;
            result.free_blocks = info.ordblks;
            result.mapped_regions = info.hblks;
            result.mapped_bytes = info.hblkhd;
            return result;
        }

        void copy_thread(TX_THREAD& thread, ThreadInfo& result)
        {
#if defined(PNM_PLATFORM_LINUX)
            _tx_linux_thread_stack_refresh(&thread);
#endif
            if (const auto* name = thread.tx_thread_name; name != nullptr) {
                std::size_t length{};
                while (length + 1 < result.name.size() && name[length] != '\0') {
                    result.name[length] = name[length];
                    ++length;
                }
                if (length + 1 == result.name.size() && name[length] != '\0') {
                    result.name[length - 1] = '~';
                }
            }
            result.prio = thread.tx_thread_priority;
            result.stack_size = static_cast<std::uint32_t>(thread.tx_thread_stack_size);
            const auto top{ reinterpret_cast<std::uintptr_t>(thread.tx_thread_stack_end) + 1U };
            auto current{ reinterpret_cast<std::uintptr_t>(thread.tx_thread_stack_ptr) };
#if defined(__arm__) || defined(__thumb__)
            // The running thread's saved pointer describes its last context switch.
            if (&thread == _tx_thread_current_ptr) {
                asm volatile("mrs %0, psp" : "=r"(current));
            }
#endif
            const auto used = [top, size = result.stack_size](std::uintptr_t pointer) {
                return static_cast<std::uint32_t>(pointer < top ?
                  std::min<std::uintptr_t>(top - pointer, size) : 0U);
            };
            result.current_stack_usage = used(current);
            result.peak_stack_usage = result.current_stack_usage;
#if defined(TX_ENABLE_STACK_CHECKING)
            auto peak{ reinterpret_cast<std::uintptr_t>(thread.tx_thread_stack_highest_ptr) };
            const auto bottom{ reinterpret_cast<std::uintptr_t>(thread.tx_thread_stack_start) };
            if (current >= bottom && current < top && (peak == 0U || current < peak)) {
                thread.tx_thread_stack_highest_ptr = reinterpret_cast<void*>(current);
                peak = current;
            }
            if (peak != 0U) {
                result.peak_stack_usage = std::max(result.peak_stack_usage, used(peak));
            }
#endif
        }

        void collect_stacks(StackInfo& result)
        {
            // Protect the created-thread list and copy names while their owners
            // cannot exit/delete them. No allocation, formatting or transport here.
            TX_INTERRUPT_SAVE_AREA
            TX_DISABLE
            result.total = static_cast<std::uint32_t>(_tx_thread_created_count);
            auto* thread{ _tx_thread_created_ptr };
            const auto count{ std::min<std::size_t>(result.total, result.threads.size()) };
            for (std::size_t index{}; thread != nullptr && index < count; ++index) {
                copy_thread(*thread, result.threads[index]);
                ++result.count;
                thread = thread->tx_thread_created_next;
            }
            TX_RESTORE
        }
    }

    MemorySnapshot collectMemory(MemoryQuery query)
    {
        MemorySnapshot result{};
        result.query = query;
#if defined(PNM_PLATFORM_LINUX)
        result.shared_runtime = true;
#endif
        switch (query) {
            case MemoryQuery::Heap: result.heap = collect_heap(); break;
            case MemoryQuery::Stack: collect_stacks(result.stack); break;
            default: throw std::invalid_argument("Unknown memory diagnostic query");
        }
        return result;
    }
}
