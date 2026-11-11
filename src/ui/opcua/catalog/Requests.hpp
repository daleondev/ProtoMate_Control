#pragma once
#include "Records.hpp"
namespace ui::opcua::catalog
{
    template<unsigned Axis>
    struct [[= Record{Axis == 2 ? Identity{"LinearMove"} : Identity{"RotaryMove"}, true}]] AxisMove
    {
        static constexpr Unit POSITION{Axis == 2 ? Unit::Millimetres : Unit::Degrees};
        static constexpr Unit SPEED{Axis == 2 ? Unit::MillimetresPerSecond : Unit::DegreesPerSecond};
        [[= Field{"Target or displacement", POSITION}, = Bounds{-1e6,1e6}]] double position{};
        [[= Field{"Zero selects configured default", SPEED}, = Bounds{0,1e6}]] double speed{};
        [[= Field{"Joint units / s^2; zero selects configured default"}, = Bounds{0,1e12}]] double acceleration{}, deceleration{};
        [[= Field{"Joint units / s^3; zero selects configured default"}, = Bounds{0,1e15}]] double jerk{};
        [[= Field{"Seconds; zero selects configured default"}, = Bounds{0,3600}]] double timeout{};
        services::Command command(services::Kind kind) const {
            return {.kind=kind, .axis=Axis, .position={position,0,0}, .speed=speed, .acceleration=acceleration,
                .deceleration=deceleration, .jerk=jerk, .timeout=timeout};
        }
    };
    template<unsigned Axis>
    struct [[= Record{Axis == 2 ? Identity{"LinearJog"} : Identity{"RotaryJog"}, true}]] AxisJog
    {
        [[= Field{"Signed joint velocity", Axis == 2 ? Unit::MillimetresPerSecond : Unit::DegreesPerSecond}, = Bounds{-1e6,1e6}]] double speed{};
        [[= Field{"Seconds before deceleration is requested"}, = Bounds{0,3600}]] double duration{1};
        [[= Field{"Requires explicit held-job heartbeat supervision"}]] bool held{};
        [[= Field{"Joint units / s^2; zero selects default"}, = Bounds{0,1e12}]] double acceleration{}, deceleration{};
        [[= Field{"Joint units / s^3; zero selects default"}, = Bounds{0,1e15}]] double jerk{};
        [[= Field{"Seconds; zero selects default"}, = Bounds{0,3600}]] double timeout{};
        services::Command command() const {return {.kind=services::Kind::AxisJog, .axis=Axis, .speed=speed,
            .acceleration=acceleration, .deceleration=deceleration, .jerk=jerk, .timeout=timeout, .duration=duration, .continuous=held};}
    };
    struct [[= Record{"RobotMove", true}]] RobotMove
    {
        [[= Field{"Joints: degrees/degrees/mm; point: mm/mm/mm"}, = Bounds{-1e6,1e6}]] std::array<double,3> position{};
        [[= Field{"Configured velocity factor; zero selects default"}, = Bounds{0,1}]] double speed{.25};
        [[= Field{"Configured acceleration factor; zero selects default"}, = Bounds{0,1}]] double acceleration{1};
        [[= Field{"Configured deceleration factor; zero selects default"}, = Bounds{0,1}]] double deceleration{1};
        [[= Field{"Configured jerk factor; zero selects default"}, = Bounds{0,1}]] double jerk{1};
        [[= Field{"Seconds; zero selects default"}, = Bounds{0,3600}]] double timeout{};
        services::Command command(services::Kind kind) const {
            return {.kind=kind, .position=position, .speed=speed, .acceleration=acceleration, .deceleration=deceleration, .jerk=jerk, .timeout=timeout};
        }
    };
    struct [[= Record{"CartesianJog", true}]] CartesianJog
    {
        [[= Field{"Cartesian direction, normalized by the existing planner"}, = Bounds{-1,1}]] std::array<double,3> direction{};
        [[= Field{"Configured velocity factor"}, = Bounds{0,1}]] double speed{.1};
        [[= Field{"Seconds before deceleration is requested"}, = Bounds{0,3600}]] double duration{1};
        [[= Field{"Requires held-job heartbeat supervision"}]] bool held{};
        [[= Field{"Configured dynamics factors; zero selects default"}, = Bounds{0,1}]] double acceleration{1}, deceleration{1}, jerk{1};
        [[= Field{"Seconds; zero selects default"}, = Bounds{0,3600}]] double timeout{};
        services::Command command() const {return {.kind=services::Kind::PointJog, .position=direction, .speed=speed,
            .acceleration=acceleration, .deceleration=deceleration, .jerk=jerk, .timeout=timeout, .duration=duration, .continuous=held};}
    };
    struct [[= Record{"MotorMove", true}]] MotorMove
    {
        [[= Field{"Shaft target or displacement", Unit::Degrees}, = Bounds{-1e9,1e9}]] double position{};
        [[= Field{"Shaft velocity", Unit::Rpm}, = Bounds{-1e6,1e6}]] double speed{};
        [[= Field{"Shaft rpm/s; zero selects default"}, = Bounds{0,1e12}]] double acceleration{}, deceleration{};
        [[= Field{"Shaft rpm/s^2; zero selects default"}, = Bounds{0,1e15}]] double jerk{};
        [[= Field{"Seconds; zero selects default"}, = Bounds{0,3600}]] double timeout{};
        [[= Field{"Slow latch speed for homing", Unit::Rpm}, = Bounds{0,1e6}]] double latch{.5};
        [[= Field{"Existing MotorBuffer enum, 0 through 5"}, = Bounds{0,5}]] std::uint32_t buffer{};
        [[= Field{"Explicitly confirm replacement; cannot replace another source's queue"}]] bool confirm_replacement{};
        services::Command command(services::Kind kind, unsigned motor) const {
            return {.kind=kind, .axis=motor, .position={position,0,0}, .speed=speed, .acceleration=acceleration,
                .deceleration=deceleration, .jerk=jerk, .timeout=timeout, .latch=latch, .buffer=buffer, .confirmed=confirm_replacement};
        }
    };
}
