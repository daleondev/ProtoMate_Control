#pragma once
#include "profiling/Trace.h"
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
#if defined(GIGA_MOTION_BENCH) || defined(GIGA_MOTION_API_TEST)
    // Test images supply the recorder. No dependency on application contracts;
    // normal firmware compiles the hooks away entirely.
    void intercoreTrace(InterCoreStage stage,
                        unsigned lane = 0,
                        std::span<const std::byte> bytes = {}) noexcept;
#elif defined(PROTOMATE_PROFILING)
    inline void intercoreTrace(InterCoreStage stage,
                               unsigned lane = 0,
                               std::span<const std::byte> bytes = {}) noexcept
    {
        profile_event(PROFILE_CAT_SYSTEM, PROFILE_ICC_STAGE, unsigned(stage), lane, bytes.size(), 0);
    }
#else
    inline void intercoreTrace(InterCoreStage, unsigned = 0, std::span<const std::byte> = {}) noexcept {}
#endif
}
