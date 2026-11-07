#include "Executor.hpp"
#include "profiling/Trace.h"

namespace io::motion
{
    // Cold path: retain the first failure before schedule storage is released.
    // No allocations, logging, or dependency on a live UI connection.
    bool Executor::failed(ScheduleFailure failure,
                          std::uint64_t cut,
                          unsigned axis,
                          double brake_distance) noexcept
    {
        if (m_status.diagnostic.failure != ScheduleFailure::None) {
            return false;
        }
        profile_failure(PROFILE_MOTION_GUARD, 0x10000U | unsigned(failure), axis, 0);
        const auto* profile = axis < 3 ? &m_nominal[axis]->profile : &*m_progress;
        const auto state = profile->at(cut / 1e7);
        m_status.diagnostic = { .failure = failure,
                                .axis = std::uint8_t(axis),
                                .cut = cut,
                                .state = state,
                                .limits = axis < 3 ? m_limits[axis] : m_progressLimits,
                                .remaining = profile->distance() - state.position,
                                .brake_distance = brake_distance };
        m_status.last_error = int(failure == ScheduleFailure::BankInUse ? std::errc::device_or_resource_busy
                                                                        : std::errc::invalid_argument);
        return false;
    }
}
