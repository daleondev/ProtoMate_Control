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
        constexpr unsigned schedule_failure_flag{ 0x10000U };
        profile_failure(PROFILE_MOTION_GUARD, schedule_failure_flag | unsigned(failure), axis, 0);
        const ::motion::Profile* profile{};
        if (axis < m_nominal.size() && m_nominal[axis]) {
            profile = &m_nominal[axis]->profile;
        }
        else if (m_progress) {
            profile = &*m_progress;
        }
        // Failure reporting must also survive partially prepared schedules.
        const auto state{ profile ? profile->at(static_cast<double>(cut) / 1e7)
                                  : ::motion::Profile::State{} };
        m_status.diagnostic = { .failure = failure,
                                .axis = std::uint8_t(axis),
                                .cut = cut,
                                .state = state,
                                .limits = axis < 3 ? m_limits[axis] : m_progressLimits,
                                .remaining = profile ? profile->distance() - state.position : 0,
                                .brake_distance = brake_distance };
        m_status.last_error = int(failure == ScheduleFailure::BankInUse ? std::errc::device_or_resource_busy
                                                                        : std::errc::invalid_argument);
        return false;
    }
}
