#include "common/icc/bridge/Frame.hpp"
#include "hal/hal.hpp"
#include "hal/util/InterCoreTrace.hpp"

#include <array>
#include <atomic>

// Each core owns a private, fixed-size trace. SWD reads these symbols without
// halting the processors. Recording stops when full; no logging or heap work is
// performed here. TIM5 is the common 1 MHz timestamp source on both processors.
extern "C"
{
    struct MotionTraceEntry
    {
        std::uint32_t time, stage, id, kind;
    };
    std::array<MotionTraceEntry, 256> motion_api_trace{};
    std::atomic<std::uint32_t> motion_api_trace_count{};
}

namespace hal::util
{
    void intercoreTrace(InterCoreStage stage, unsigned lane, std::span<const std::byte> bytes) noexcept
    {
        if (bytes.size() < icc::util::HEADER_BYTES ||
            bytes[0] != std::byte{ icc::util::FRAME_VERSION }) {
            return;
        }
        const auto kind = std::to_integer<unsigned>(bytes[1]);
        if (kind == unsigned(icc::util::FrameKind::Topic) ||
            kind >= unsigned(icc::util::FrameKind::Hello)) {
            return;
        }
        std::uint32_t id{};
        for (unsigned i = 0; i < 4; ++i) {
            id |= std::to_integer<std::uint32_t>(bytes[4 + i]) << (8 * i);
        }
        // Frame-bearing transport calls run on this core's single ICC worker.
        // ISR/notification hooks have no frame and returned above, so recording
        // does not need to mask interrupts or extend STEP interrupt latency.
        const auto n = motion_api_trace_count.load(std::memory_order_relaxed);
        if (n < motion_api_trace.size()) {
            motion_api_trace[n] = { TIM5->CNT, unsigned(stage) | (lane << 16), id, kind };
            motion_api_trace_count.store(n + 1, std::memory_order_release);
        }
    }
}
