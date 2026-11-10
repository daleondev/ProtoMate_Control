#include "Trace.hpp"
#include "Bench.hpp"
#include "hal/stm32/InterruptGuard.hpp"
#include "hal/util/InterCoreTrace.hpp"
#include <cstdio>

namespace
{
    __attribute__((section(".motion_bench_trace"), aligned(32))) motion_bench::TraceStore storage;
    std::atomic_bool enabled{};
}
namespace hal::util
{
    void intercore_trace(InterCoreStage stage, unsigned lane, std::span<const std::byte> bytes) noexcept
    {
        if (!enabled.load(std::memory_order_relaxed)) {
            return;
        }
        const stm32::InterruptGuard guard{};
        if (!enabled.load(std::memory_order_relaxed)) {
            return;
        }
        auto& entry = storage.entries[storage.count++ % storage.entries.size()];
        entry = { motion_bench::now(),         motion_bench::cycles(),
                  std::to_underlying(stage),   lane,
                  std::uint32_t(bytes.size()), {} };
        std::copy_n(bytes.begin(), std::min(bytes.size(), entry.header.size()), entry.header.begin());
    }
}
namespace motion_bench
{
    void beginTrace(bool active)
    {
        const hal::stm32::InterruptGuard guard{};
        storage.count = 0;
        enabled = active;
    }
    void endTrace()
    {
        const hal::stm32::InterruptGuard guard{};
        enabled = false;
    }
    void printTrace(unsigned side)
    {
        const auto& data = *reinterpret_cast<const TraceStore*>(side == 0 ? 0xC0420000U : 0xC0460000U);
        const auto first = data.count > data.entries.size() ? data.count - data.entries.size() : 0;
        for (unsigned i = first; i < data.count; ++i) {
            const auto& e = data.entries[i % data.entries.size()];
            std::printf("{\"test\":\"trace\",\"core\":%u,\"index\":%u,\"time\":%lu,\"cycles\":%lu,\"stage\":%"
                        "lu,\"lane\":%lu,\"bytes\":%lu,\"header\":\"",
                        side ? 7 : 4,
                        i,
                        (unsigned long)e.time,
                        (unsigned long)e.cycles,
                        (unsigned long)e.stage,
                        (unsigned long)e.lane,
                        (unsigned long)e.size);
            for (auto byte : e.header) {
                std::printf("%02x", std::to_integer<unsigned>(byte));
            }
            std::puts("\"}");
            std::this_thread::sleep_for(2ms);
        }
    }
}
