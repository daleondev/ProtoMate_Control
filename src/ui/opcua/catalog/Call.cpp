#include "Call.hpp"
#include "Wire.hpp"
#include "ui/app/Services.hpp"
#include <algorithm>

namespace ui::opcua::catalog
{
    namespace {
        bool same(const services::Command& a, const services::Command& b) {
            return a.kind == b.kind && a.axis == b.axis && a.position == b.position && a.speed == b.speed &&
                a.acceleration == b.acceleration && a.deceleration == b.deceleration && a.jerk == b.jerk &&
                a.timeout == b.timeout && a.duration == b.duration && a.latch == b.latch && a.buffer == b.buffer &&
                a.continuous == b.continuous && a.confirmed == b.confirmed && a.job == b.job;
        }
        template<std::size_t N, std::size_t M> void copy(std::array<char,N>& out, const std::array<char,M>& in) {
            const auto end = std::ranges::find(in, '\0');
            const auto length = std::min(N-1, static_cast<std::size_t>(end-in.begin()));
            std::copy_n(in.begin(), length, out.begin()); out[length] = 0;
        }
    }
    std::uint64_t Call::begin()
    {
        if (m_services.remoteAuthorized(m_session.source)) return m_session.source;
        const auto source{m_services.openRemote()};
        if (!source) check(UA_STATUSCODE_BADUSERACCESSDENIED);
        m_session.source = source; m_session.lastRequest = 0; m_session.next = 0;
        m_session.previous = {};
        return source;
    }
    bool Call::heartbeat(std::uint64_t generation, const std::array<std::uint64_t,8>& held)
    {
        if (!generation || generation != m_session.source) check(UA_STATUSCODE_BADINVALIDSTATE);
        std::array<std::uint64_t,8> active{};
        auto end{std::copy_if(held.begin(), held.end(), active.begin(), [](auto id) {return id != 0;})};
        return m_services.renewRemote(generation, {active.begin(), end});
    }
    Receipt Call::submit(std::uint64_t generation, std::uint64_t request, const services::Command& command)
    {
        if (!generation || generation != m_session.source || !m_services.remoteAuthorized(m_session.source)) check(UA_STATUSCODE_BADUSERACCESSDENIED);
        if (!request) check(UA_STATUSCODE_BADSEQUENCENUMBERINVALID);
        if (request <= m_session.lastRequest) {
            for (const auto& entry : m_session.previous) if (entry.request == request) {
                if (!same(command, entry.command)) check(UA_STATUSCODE_BADINVALIDARGUMENT);
                return entry.receipt;
            }
            check(UA_STATUSCODE_BADSEQUENCENUMBERINVALID);
        }
        // Consume the request before submission, including explicit rejections.
        // A timeout must never make an uncertain retry execute a second time.
        m_session.lastRequest = request;
        auto& entry{m_session.previous[m_session.next++ % m_session.previous.size()]};
        entry = {}; entry.request = request; entry.command = command;
        entry.receipt = Receipt::from(m_services.submit(command, m_session.source));
        return entry.receipt;
    }
    Receipt Call::result(std::uint64_t operation)
    {
        if (!m_session.source) check(UA_STATUSCODE_BADNODATA);
        const auto result{m_services.operation(operation, m_session.source)};
        if (!result) check(UA_STATUSCODE_BADNODATA);
        return Receipt::from(*result);
    }
    JobPage Call::jobs(std::uint32_t offset, bool previews)
    {
        if (offset > 32) check(UA_STATUSCODE_BADOUTOFRANGE);
        JobPage page{}; page.offset = offset;
        std::array<services::DetailedJob,1> rows{};
        while (page.count < page.records.size()) {
            const auto count{m_services.detailedJobs(offset + page.count, rows, previews)};
            for (unsigned i{}; i < count; ++i) {
                auto& out{page.records[page.count++]}; const auto& in{rows[i]};
                out.id = in.id; out.source = in.source; out.state = in.state; out.family = in.family; out.axes = in.axes;
                out.terminal = in.terminal; out.preview = in.preview; out.busy_owner = in.busy;
                out.duration = in.seconds; out.endpoint_steps = in.endpoint; copy(out.detail, in.detail);
                out.requested = in.requested; out.relative = in.relative;
                out.requested_space = in.requested_space; out.requested_units = in.requested_units;
            }
            if (count < rows.size()) break;
        }
        return page;
    }
    DiagnosticPage Call::diagnostics(std::uint32_t offset)
    {
        if (offset > 64) check(UA_STATUSCODE_BADOUTOFRANGE);
        services::DiagnosticPage source{}; m_services.diagnostics(offset, source);
        DiagnosticPage out{};
        out.count = source.count; out.total = source.total; out.omitted = source.omitted; out.allocator = source.allocator;
        out.available = source.available; out.busy = source.busy; out.io = source.io; out.stacks = source.stacks; out.shared = source.shared;
        out.age = source.age_ms; out.capacity = source.capacity; out.used = source.used; copy(out.error, source.error);
        for (unsigned i{}; i < out.count; ++i) {copy(out.rows[i].name, source.rows[i].name); out.rows[i].values = source.rows[i].values;}
        return out;
    }
    PreviewResult Call::preview(std::uint64_t job)
    {
        const auto value{m_services.previewResult(job)};
        return {value.available, value.state, value.error, value.segments, value.id, value.source,
            value.duration, value.approximation_mm, value.quantization_mm, value.endpoint_joints,
            value.endpoint_tcp, value.peak_velocity, value.peak_acceleration, value.peak_jerk};
    }
    bool Call::refreshDiagnostics(bool io, bool stacks) {return m_services.refreshDiagnostics(io, stacks);}
    MessagePage Call::messages(std::uint32_t offset)
    {
        if (offset > 8) check(UA_STATUSCODE_BADOUTOFRANGE);
        MessagePage out{};
        std::array<services::Message,8> rows{};
        out.count = m_services.messages(offset, rows, &out.revision);
        for (unsigned i{}; i < out.count; ++i) {
            const auto& row{rows[i]};
            out.records[i] = {row.sequence, row.time_ms, row.operation, row.severity, row.acknowledged, row.text};
        }
        return out;
    }
    Graphics Call::graphics()
    {
        const auto value{m_services.graphics()};
        return {value.available, value.brightness, value.frames, value.presented, value.underruns,
            value.dma_errors, value.render_failures, value.queue_overflow, value.maximum_render_us,
            value.scene_presented, value.scene_latency_us};
    }
    FrontendQueues Call::queues()
    {
        const auto value{m_services.queues()};
        return {value.pending, value.urgent, value.capacity, value.urgent_capacity, value.peak, value.rejected};
    }
    Configuration Call::configuration()
    {
        const auto value{m_services.configuration()};
        Configuration out{};
        template for (constexpr auto field : fields<Configuration>()) {
            constexpr auto source{[] consteval {
                for (auto candidate : fields<services::Configuration>())
                    if (std::meta::identifier_of(candidate) == std::meta::identifier_of(field)) return candidate;
                throw "Configuration presentation field missing";
            }()};
            out.[:field:] = value.[:source:];
        }
        return out;
    }
    MotorDefaults Call::motorDefaults(unsigned motor)
    {
        if (motor >= 3) check(UA_STATUSCODE_BADOUTOFRANGE);
        services::Snapshot value{}; m_services.read(value);
        const auto& axis{value.motor(motor)};
        return {value.connected && value.fresh && !value.failed, axis.acceleration, axis.deceleration, axis.jerk};
    }
    NetworkDiagnostics Call::network()
    {
        const auto n{m_services.network()};
        return {n.wifi, n.connected, n.busy, n.saved, n.state, n.address, n.error, n.capacity, n.used, n.peak,
            n.failures, n.packets, n.available_packets, n.empty_packet_requests, n.packet_suspensions, n.invalid_packet_releases, n.receive_packets, n.receive_available, n.receive_empty_requests};
    }
}
