#include "Profiling.hpp"
#include "Memory.hpp"
#include <chrono>

namespace diagnostics
{
    ProfileResponse control_profile(ProfileRequest request)
    {
        std::uint32_t result{};
        switch (request.operation) {
            case ProfileOperation::Status:
                break;
            case ProfileOperation::Arm:
                result = static_cast<std::uint32_t>(profile_arm(request.preset, request.capture));
                break;
            case ProfileOperation::Freeze:
                profile_freeze(PROFILE_MANUAL);
                break;
            default:
                result = 1;
        }
        return { .result = result, .status = profile_status() };
    }
    void poll_profiling()
    {
        profile_service();
        // On Linux UI owns the shared runtime's collection; the IO loop does
        // not call this function, avoiding a second collector and timer race.
        static auto next{ std::chrono::steady_clock::time_point{} };
        const auto now{ std::chrono::steady_clock::now() };
        if (now < next) {
            return;
        }
        next = now + std::chrono::seconds{ 1 };
        sample_profiling_memory();
    }
}
