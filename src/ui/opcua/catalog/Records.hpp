#pragma once
#include "Annotations.hpp"
#include "ui/app/services/Types.hpp"

namespace ui::opcua::catalog
{
    struct [[= Record{"FrontendQueues"}]] FrontendQueues
    {
        [[= Field{}]] std::uint32_t pending{}, urgent{}, capacity{}, urgent_capacity{}, peak{};
        [[= Field{}]] std::uint64_t rejected{};
    };
    struct [[= Record{"NetworkDiagnostics"}]] NetworkDiagnostics
    {
        [[= Field{}]] bool wifi{}, connected{}, busy{}, saved{};
        [[= Field{"Network manager state"}]] std::uint32_t state{};
        [[= Field{"IPv4 in host integer form, most significant octet first"}]] std::uint32_t address{};
        [[= Field{"Driver result"}]] std::int32_t error{};
        [[= Field{"Dedicated network allocator bytes", Unit::Bytes}]] std::uint64_t capacity{}, used{}, peak{};
        [[= Field{}]] std::uint64_t failures{}, packets{}, available_packets{}, empty_packet_requests{}, packet_suspensions{}, invalid_packet_releases{};
    };
    struct [[= Record{"ApplicationMessage"}]] Message
    {
        [[= Field{}]] std::uint64_t sequence{}, time_ms{}, operation{};
        [[= Field{}]] services::Severity severity{};
        [[= Field{}]] bool acknowledged{};
        [[= Field{}]] std::array<char,160> text{};
    };
    struct [[= Record{"MessagePage"}]] MessagePage
    {
        [[= Field{}]] std::uint32_t count{};
        [[= Field{}]] std::uint64_t revision{};
        [[= Field{}]] std::array<Message,8> records{};
    };
    struct [[= Record{"Graphics"}]] Graphics
    {
        [[= Field{}]] bool available{};
        [[= Field{}]] std::uint32_t brightness{};
        [[= Field{}]] std::uint64_t frames{}, presented{}, underruns{}, dma_errors{}, render_failures{},
            queue_overflow{}, maximum_render_us{}, scene_presented{}, scene_latency_us{};
    };
    struct [[= Record{"MechanicalConfiguration"}]] Configuration
    {
        [[= Field{}]] bool available{}, calibrated{}, simulation{}, kinematics{}, neutral_available{};
        [[= Field{}]] std::uint64_t identity{}, hardware{};
        [[= Field{"One-based axis/motor indices"}]] std::array<std::uint32_t,3> motor_for_axis{}, home_order{};
        [[= Field{}]] std::array<bool,3> opposite{}, home_configured{}, home_positive{};
        [[= Field{"Motor revolutions per output revolution"}]] std::array<double,3> reduction{};
        [[= Field{"Axis 1/2 degrees, Axis 3 millimetres"}]] std::array<double,3> reference{}, minimum{}, maximum{}, neutral{};
        [[= Field{"Joint units per second"}]] std::array<double,3> velocity{};
        [[= Field{"Joint units per second squared"}]] std::array<double,3> acceleration{}, deceleration{};
        [[= Field{"Joint units per second cubed"}]] std::array<double,3> jerk{};
        [[= Field{"M1/M2/M3 electrical step angles", Unit::Degrees}]] std::array<double,3> motor_step_degrees{};
        [[= Field{"Millimetres", Unit::Millimetres}]] std::array<double,3> base_mm{}, tool_mm{}, work_mm{}, workspace_min_mm{}, workspace_max_mm{};
        [[= Field{"Shaft rpm", Unit::Rpm}]] std::array<double,3> home_seek_rpm{}, home_latch_rpm{};
        [[= Field{"Seconds"}]] std::array<double,3> home_timeout_s{};
        [[= Field{"Arm lengths", Unit::Millimetres}]] std::array<double,2> links_mm{};
        [[= Field{"Millimetres per screw revolution"}]] double screw_lead_mm{};
        [[= Field{"Degrees", Unit::Degrees}]] double base_yaw_deg{}, tool_yaw_deg{}, work_yaw_deg{};
        [[= Field{"Numerical approximation budget; excludes quantization and mechanics", Unit::Millimetres}]] double approximation_mm{};
    };
    struct [[= Record{"MotorDefaults"}]] MotorDefaults
    {
        [[= Field{}]] bool available{};
        [[= Field{"Shaft rpm/s"}]] double acceleration{}, deceleration{};
        [[= Field{"Shaft rpm/s squared"}]] double jerk{};
    };
    struct [[= Record{"Receipt"}]] Receipt
    {
        [[= Field{}]] std::uint64_t operation{};
        [[= Field{}]] std::uint64_t job{};
        [[= Field{}]] bool submitted{};
        [[= Field{}]] services::Outcome::Phase phase{};
        [[= Field{}]] std::array<char,160> reason{};
        static Receipt from(const services::Outcome& value)
        { return {value.operation, value.job, value.accepted, value.phase, value.message}; }
    };
    struct [[= Record{"Job"}]] Job
    {
        [[= Field{}]] std::uint64_t id{};
        [[= Field{}]] std::uint64_t source{};
        [[= Field{}]] std::uint32_t state{};
        [[= Field{}]] std::uint32_t family{};
        [[= Field{}]] std::uint32_t axes{};
        [[= Field{}]] bool terminal{};
        [[= Field{}]] bool preview{};
        [[= Field{}]] std::uint64_t busy_owner{};
        [[= Field{}]] double duration{};
        [[= Field{"Quantized M1/M2/M3 pulse positions"}]] std::array<double,3> endpoint_steps{};
        [[= Field{"Requested values in the accompanying units and coordinate space"}]] std::array<double,3> requested{};
        [[= Field{}]] std::array<char,32> requested_space{}, requested_units{};
        [[= Field{}]] bool relative{};
        [[= Field{}]] std::array<char,96> detail{};
    };
    struct [[= Record{"JobPage"}]] JobPage
    {
        [[= Field{}]] std::uint32_t offset{};
        [[= Field{}]] std::uint32_t count{};
        [[= Field{}]] std::array<Job,8> records{};
    };
    struct [[= Record{"DiagnosticRow"}]] DiagnosticRow
    {
        [[= Field{}]] std::array<char,32> name{};
        [[= Field{}]] std::array<std::uint64_t,4> values{};
    };
    struct [[= Record{"DiagnosticPage"}]] DiagnosticPage
    {
        [[= Field{}]] std::uint32_t count{}, total{}, omitted{}, allocator{};
        [[= Field{}]] bool available{}, busy{}, io{}, stacks{}, shared{};
        [[= Field{}]] std::uint64_t age{}, capacity{}, used{};
        [[= Field{}]] std::array<char,48> error{};
        [[= Field{}]] std::array<DiagnosticRow,6> rows{};
    };
}
