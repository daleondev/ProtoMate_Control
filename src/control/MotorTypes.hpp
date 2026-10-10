#pragma once
#include <pneumo/units.hpp>
namespace control
{
    using namespace pnm::units::literals;
    enum class MotorResult
    {
        Completed,
        Stopped,
        TimedOut,
        Rejected,
        Faulted
    };
    enum class BufferMode
    {
        Aborting,
        Buffered,
        BlendingLow,
        BlendingPrevious,
        BlendingNext,
        BlendingHigh
    };
    struct MotionDefaults
    {
        pnm::units::AngularAcceleration acceleration{ 3600_deg_s2 };
        pnm::units::AngularAcceleration deceleration{ 3600_deg_s2 };
        pnm::units::AngularJerk jerk{ 36000_deg_s3 };
    };
}
