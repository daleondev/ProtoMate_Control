#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace hal::util
{
    enum class InterCoreStage : std::uint32_t
    {
        PublishBegin = 1,
        Published,
        ReceiveBegin,
        Received,
        Interrupt,
        NotificationWorker,
        TransportSend,
        TransportSent,
        TransportReceived,
        BridgeDispatch,
        Handler,
        Yield,
        Submission,
        Enqueued,
        Completed
    };
#if defined(GIGA_MOTION_BENCH)
    // Benchmark supplies the recorder. No dependency on application contracts;
    // normal firmware compiles the hooks away entirely.
    void intercoreTrace(InterCoreStage stage,
                        unsigned lane = 0,
                        std::span<const std::byte> bytes = {}) noexcept;
#else
    inline void intercoreTrace(InterCoreStage, unsigned = 0, std::span<const std::byte> = {}) noexcept {}
#endif
}
