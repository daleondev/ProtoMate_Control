#include "GuiBackend.hpp"
#include "common/icc/catalog/services.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace ui
{
    namespace hmi = gui::hmi;
    namespace tr = ::motion::trajectory;
    namespace
    {
        constexpr double RAD = std::numbers::pi / 180;
        constexpr const char* jobNames[]{ "Submitting", "Planning", "Preparing", "Accepted",
                                          "Running",    "Stopping", "Completed", "Stopped",
                                          "Timed out",  "Rejected", "Faulted",   "Unknown" };
        template<std::size_t N>
        void text(std::array<char, N>& out, const char* value)
        {
            std::snprintf(out.data(), out.size(), "%s", value);
        }
        motion::JointPosition joints(const std::array<double, 3>& p)
        {
            return { pnm::units::Angle::create(p[0] * RAD),
                     pnm::units::Angle::create(p[1] * RAD),
                     pnm::units::Distance::create(p[2] / 1000) };
        }
        motion::CartesianPosition point(const std::array<double, 3>& p)
        {
            return { pnm::units::Distance::create(p[0] / 1000),
                     pnm::units::Distance::create(p[1] / 1000),
                     pnm::units::Distance::create(p[2] / 1000) };
        }
    }
    GuiBackend::GuiBackend(pnm::msg::Bus& bus,
                           motion::Client& client,
                           motion::Robot& robot,
                           SystemState& system)
      : m_client{ client }
      , m_robot{ robot }
      , m_system{ system }
      , m_memory{ bus.service<^^icc::Services::io_memory>() }
    {
#ifdef HAL_PLATFORM_LINUX
        m_state.simulation = true;
#endif
        text(m_state.condition, "Awaiting IO telemetry");
        m_client.setObserver([this] { update(); });
    }
    GuiBackend::~GuiBackend() { m_client.setObserver({}); }
    void GuiBackend::connection(icc::ConnectionStatus state, const char* detail)
    {
        std::scoped_lock lock{ m_mutex };
        m_state.connected = state.state == icc::ConnectionState::Connected;
        if (state.state == icc::ConnectionState::Failed ||
            state.reason == icc::ConnectionReason::PeerStopped) {
            m_state.failed = true;
            m_state.fresh = false;
            std::snprintf(m_state.condition.data(),
                          m_state.condition.size(),
                          "ICC failure %u: restart required; physical stop unconfirmed",
                          unsigned(state.reason));
            if (detail && *detail)
                std::snprintf(m_state.condition.data(),
                              m_state.condition.size(),
                              "ICC failure %u: %.96s; restart required",
                              unsigned(state.reason),
                              detail);
            m_holding = false;
            ++m_gesture;
        }
    }
    void GuiBackend::read(hmi::Snapshot& result)
    {
        std::scoped_lock lock{ m_mutex };
        result = m_state;
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
        if (result.receivedUs)
            result.ageMs = (us - result.receivedUs) / 1000;
        if (result.ageMs > 500) {
            result.fresh = false;
            if (!result.failed)
                text(result.condition, "Telemetry stale: pose frozen; motion unavailable");
        }
    }
    void GuiBackend::outcome(std::uint64_t id, bool ok, const char* message, std::uint64_t job)
    {
        std::scoped_lock lock{ m_mutex };
        // Operation IDs are allocation order, not completion order. Each update
        // gets a new presentation revision so late responses remain visible.
        m_state.outcome.operation = m_next++;
        m_state.outcome.accepted = ok;
        m_state.outcome.severity = ok ? hmi::Severity::Info : hmi::Severity::Error;
        m_state.outcome.job = job;
        std::snprintf(m_state.outcome.message.data(),
                      m_state.outcome.message.size(),
                      "%s (#%llu)%s",
                      message,
                      (unsigned long long)(job ? job : id),
                      ok ? "" : "; no automatic retry");
    }
    void GuiBackend::update()
    {
        hmi::Snapshot next;
        {
            std::scoped_lock lock{ m_mutex };
            next = m_state;
        }
        const auto system = m_system.snapshot();
        next.uiState = unsigned(system.ui);
        next.ioState = unsigned(system.io.value_or(app::State::Starting));
        next.combinedState = unsigned(system.combined);
        next.ioStateFresh = system.io_fresh;
        try {
            const auto state = m_client.state();
            next.fresh = true;
            next.sequence = state.sequence;
            next.boot = state.boot;
            next.configuration = state.configuration;
            next.receivedUs =
              std::chrono::duration_cast<std::chrono::microseconds>(m_client.receivedAt().time_since_epoch())
                .count();
            next.enabled = state.enabled;
            next.initialized = state.ready;
            next.family = unsigned(state.family);
            m_robot.requestPresentationModel(state.boot, state.configuration);
            const auto model = m_robot.presentationModel();
            next.configured = bool(model);
            next.robotConfigured = model && model->hasKinematics();
            next.poseValid = false;
            next.feedbackValid = false;
            bool allReferenced = true, allFeedback = true, fault = false, busy = false;
            tr::Positions commanded{}, feedback{};
            for (unsigned i = 0; i < 3; ++i) {
                const auto& motor = state.motors[i];
                auto& a = next.axes[i];
                a.shaft = motor.position;
                a.rpm = motor.velocity;
                a.shaftFeedback = motor.actual_position;
                a.reference = motor.switch_active;
                a.referenced = motor.referenced;
                a.valid = motor.position_valid;
                a.velocityValid = motor.velocity_valid;
                a.ready = motor.driver_ready;
                a.fault = motor.fault_active || motor.fault_latched;
                a.faultCode = motor.fault;
                a.warning = motor.warning;
                a.encoder = motor.feedback == ::motion::MotorFeedback::Encoder;
                a.owner = state.owners[i];
                next.jointForMotor[i] = i;
                a.acceleration = motor.acceleration;
                a.deceleration = motor.deceleration;
                a.jerk = motor.jerk;
                text(a.driver, motor.driver.c_str());
                allReferenced &= a.referenced;
                allFeedback &= a.valid;
                fault |= a.fault;
                busy |= a.owner != 0;
                if (model) {
                    commanded[i] = motor.position * RAD / model->hardware().step_radians[i];
                    feedback[i] = motor.actual_position * RAD / model->hardware().step_radians[i];
                }
            }
            if (model) {
                const auto q = model->toJoints(commanded);
                next.commanded = model->joints(q);
                const auto aq = model->toJoints(feedback);
                next.feedback = model->joints(aq);
                next.poseValid = allReferenced;
                next.feedbackValid = allReferenced && allFeedback;
                const auto limits = model->jointLimits();
                // Axis presentation indexes joints, remapping the corresponding motor metadata.
                const auto motorAxes = next.axes;
                for (unsigned i = 0; i < 3; ++i) {
                    const auto motor = model->configuration().motor_for_joint[i];
                    auto& a = next.axes[i];
                    a = motorAxes[motor];
                    next.jointForMotor[motor] = i;
                    const double unit = i == 2 ? 1000 : 1 / RAD;
                    a.commanded = next.commanded[i] * unit;
                    a.feedback = next.feedback[i] * unit;
                    a.minimum = limits[i].minimum * unit;
                    a.maximum = limits[i].maximum * unit;
                    a.maxSpeed = limits[i].velocity * unit;
                }
                if (model->hasKinematics())
                    if (const auto fk = model->kinematics().forward(q)) {
                        next.tcp = { fk->position.x.get() * 1000,
                                     fk->position.y.get() * 1000,
                                     fk->position.z.get() * 1000 };
                        next.yaw = fk->yaw.get() / RAD;
                    }
                if (model->hasKinematics())
                    if (const auto fk = model->kinematics().forward(aq)) {
                        next.feedbackTcp = { fk->position.x.get() * 1000,
                                             fk->position.y.get() * 1000,
                                             fk->position.z.get() * 1000 };
                        next.feedbackYaw = fk->yaw.get() / RAD;
                    }
                const auto& config = model->configuration();
                std::snprintf(next.details.data(),
                              next.details.size(),
                              "%s configuration\nMotor mapping %u / %u / %u\nLimits and transmission: read "
                              "only\nM1 encoder; M2/M3 INDEX are phase estimates",
                              config.purpose == motion::RobotConfiguration::Purpose::Simulation
                                ? "Native simulation"
                                : "Verified physical",
                              config.motor_for_joint[0] + 1,
                              config.motor_for_joint[1] + 1,
                              config.motor_for_joint[2] + 1);
            }
            const char* condition = fault            ? "Driver fault: disable and inspect diagnostics"
                                    : !state.ready   ? "Initialization required (does not enable)"
                                    : !state.enabled ? "Disabled: initialize, then explicitly enable"
                                    : !model         ? "Mechanical calibration required for axes / robot"
                                    : !allReferenced ? "Unreferenced: home the required axes"
                                    : busy           ? "Motion active: Stop remains available"
                                                     : "Ready";
            if (!next.failed)
                text(next.condition, condition);
        } catch (const std::exception& e) {
            next.fresh = false;
            if (!next.failed)
                text(next.condition, e.what());
        }
        {
            std::scoped_lock lock{ m_mutex };
            // Connection/outcome can change concurrently while we collect values.
            next.connected = m_state.connected;
            next.failed = m_state.failed;
            next.outcome = m_state.outcome;
            if (m_state.failed) {
                next.fresh = false;
                next.condition = m_state.condition;
            }
            m_state = next;
        }
        std::optional<unsigned> memory;
        {
            std::scoped_lock lock{ m_mutex };
            if (!m_memoryCall)
                memory = std::exchange(m_memoryRequest, {});
        }
        if (memory) {
            if (*memory & 1U) {
                m_memoryCall.emplace(m_memory.request((*memory & 2U) ? diagnostics::MemoryQuery::Stack
                                                                     : diagnostics::MemoryQuery::Heap,
                                                      std::chrono::seconds{ 3 }));
                m_memoryReady = pnm::msg::watch(*m_memoryCall, [this] { m_client.notify(); });
            }
            else {
                const auto data = diagnostics::collectMemory((*memory & 2U) ? diagnostics::MemoryQuery::Stack
                                                                            : diagnostics::MemoryQuery::Heap);
                outcome(m_memoryOperation, true, "UI diagnostics updated");
                std::scoped_lock lock{ m_mutex };
                m_memoryData = data;
                m_memoryTime = std::chrono::steady_clock::now();
                m_memoryAvailable = true;
                m_memoryIO = false;
                m_memoryError = {};
                m_memoryBusy = false;
            }
        }
        if (m_memoryCall && m_memoryCall->ready()) {
            auto data = m_memoryCall->get();
            m_memoryReady = {};
            m_memoryCall.reset();
            outcome(
              m_memoryOperation, bool(data), data ? "IO diagnostics updated" : "IO diagnostics unavailable");
            std::scoped_lock lock{ m_mutex };
            m_memoryIO = true;
            m_memoryAvailable = bool(data);
            m_memoryError = {};
            if (data) {
                m_memoryData = *data;
                m_memoryTime = std::chrono::steady_clock::now();
            }
            else
                std::snprintf(m_memoryError.data(),
                              m_memoryError.size(),
                              "IO request failed (%u)",
                              unsigned(data.error()));
            m_memoryBusy = false;
        }
    }
    void GuiBackend::diagnostics(unsigned offset, hmi::DiagnosticPage& out)
    {
        std::scoped_lock lock{ m_mutex };
        out = {};
        out.busy = m_memoryBusy;
        out.available = m_memoryAvailable;
        out.io = m_memoryIO;
        out.error = m_memoryError;
        if (!out.available)
            return;
        out.ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                          m_memoryTime)
                      .count();
        out.stacks = m_memoryData.query == diagnostics::MemoryQuery::Stack;
        out.shared = m_memoryData.shared_runtime;
        if (out.stacks) {
            const auto& stacks = m_memoryData.stack;
            out.total = std::min<unsigned>(stacks.count, stacks.threads.size());
            out.omitted = stacks.total > out.total ? stacks.total - out.total : 0;
            for (unsigned i = offset; i < out.total && out.count < out.rows.size(); ++i) {
                auto& row = out.rows[out.count++];
                const auto& t = stacks.threads[i];
                std::snprintf(row.name.data(), row.name.size(), "%.*s", int(t.name.size()), t.name.data());
                row.values = { t.prio, t.stack_size, t.current_stack_usage, t.peak_stack_usage };
            }
        }
        else {
            const auto& h = m_memoryData.heap;
            constexpr const char* names[]{ "Capacity [B]",   "Reserved [B]",   "Used [B]",
                                           "Available [B]",  "Releasable [B]", "Free blocks",
                                           "Mapped regions", "Mapped [B]",     "Usage [%]" };
            const auto capacity = h.capacity ? h.capacity : h.reserved;
            const std::uint64_t values[]{
                h.capacity,       h.reserved,     h.used,
                h.available,      h.releasable,   h.free_blocks,
                h.mapped_regions, h.mapped_bytes, capacity ? h.used * 100 / capacity : 0
            };
            out.total = 9;
            out.allocator = unsigned(h.allocator);
            out.capacity = capacity;
            out.used = h.used;
            for (unsigned i = offset; i < out.total && out.count < out.rows.size(); ++i) {
                auto& row = out.rows[out.count++];
                text(row.name, names[i]);
                row.values[0] = values[i];
            }
        }
    }
    hmi::Outcome GuiBackend::submit(const hmi::Command& c)
    {
        hmi::Outcome reply;
        hmi::Snapshot state;
        {
            std::scoped_lock lock{ m_mutex };
            reply.operation = m_next++;
        }
        read(state);
        try {
            const bool local = c.kind == hmi::Kind::MemoryUI || c.kind == hmi::Kind::MemoryUIStack ||
                               c.kind == hmi::Kind::CancelPreview;
            const bool stop = c.kind == hmi::Kind::Stop || c.kind == hmi::Kind::Abort ||
                              c.kind == hmi::Kind::Disable || c.kind == hmi::Kind::Cancel ||
                              c.kind == hmi::Kind::AxisStop || c.kind == hmi::Kind::MotorAbort;
            if (!local && (!state.connected || state.failed))
                throw std::runtime_error("IO disconnected; stop unconfirmed; restart required");
            if (!local && !stop && !state.fresh)
                throw std::runtime_error("IO telemetry stale; command not submitted");
            if (c.axis >= 3 || c.buffer > 5)
                throw std::invalid_argument("Invalid axis or buffer mode");
            for (double value : { c.position[0],
                                  c.position[1],
                                  c.position[2],
                                  c.speed,
                                  c.acceleration,
                                  c.deceleration,
                                  c.jerk,
                                  c.timeout,
                                  c.duration,
                                  c.latch })
                if (!std::isfinite(value))
                    throw std::invalid_argument("Nonfinite motion parameter");
            using enum hmi::Kind;
            auto operation = [&](::motion::MotorRequest request) {
                const auto id = reply.operation;
                if (!m_client.requestAsync(request, [this, id](auto result) {
                    if (!result) {
                        outcome(id, false, "Control transport failed; hardware outcome unknown");
                        return;
                    }
                    outcome(id,
                            result->ok,
                            result->message.empty()
                              ? (result->ok ? "Control acknowledged" : "Control rejected")
                              : result->message.c_str());
                }))
                    throw std::runtime_error(
                      "Control queue full or disconnected; retry after acknowledgement");
            };
            switch (c.kind) {
                case Initialize:
                    operation({ .operation = ::motion::MotorOperation::Initialize });
                    break;
                case Enable:
                    operation({ .operation = ::motion::MotorOperation::Enable });
                    break;
                case Disable:
                    release();
                    m_client.stopAsync(7, true);
                    operation({ .operation = ::motion::MotorOperation::Disable });
                    break;
                case Stop:
                    release();
                    m_client.stopAsync();
                    break;
                case Abort:
                    release();
                    m_client.stopAsync(7, true);
                    break;
                case Cancel:
                    m_client.cancelAsync(c.job);
                    break;
                case MotorAbort:
                    m_client.stopAsync(std::uint8_t(1U << c.axis), true);
                    break;
                case AxisStop: {
                    auto model = m_robot.presentationModel();
                    if (!model)
                        throw std::runtime_error("Axis mapping unavailable; use global Stop");
                    m_client.stopAsync(std::uint8_t(1U << model->configuration().motor_for_joint[c.axis]));
                    break;
                }
                case MotorDefaults:
                case MotorSpeed:
                    operation({ .operation = c.kind == MotorDefaults ? ::motion::MotorOperation::Defaults
                                                                     : ::motion::MotorOperation::Speed,
                                .motor = static_cast<::motion::MotorId>(c.axis),
                                .velocity = c.speed,
                                .acceleration = c.acceleration,
                                .deceleration = c.deceleration,
                                .jerk = c.jerk,
                                .dynamics_fields = std::uint8_t(c.kind == MotorDefaults ? 7 : 0) });
                    break;
                case MotorRelative:
                case MotorAbsolute:
                case MotorHome:
                    if (!c.confirmed && c.buffer == 0) {
                        bool occupied = state.motor(c.axis).owner != 0;
                        std::array<motion::Client::JobInfo, 4> pending{};
                        for (unsigned offset = 0;; offset += 4) {
                            const auto count = m_client.copyJobs(offset, pending);
                            for (unsigned i = 0; i < count; ++i)
                                occupied |= pending[i].state < motion::Client::JobState::Completed &&
                                            (pending[i].axes & (1U << c.axis));
                            if (count < 4)
                                break;
                        }
                        if (occupied)
                            throw std::runtime_error("Active raw motion requires replacement confirmation");
                    }
                    reply.job =
                      m_client.submit({ .motor = static_cast<::motion::MotorId>(c.axis),
                                        .motion = c.kind == MotorHome       ? ::motion::MotorMotion::Home
                                                  : c.kind == MotorAbsolute ? ::motion::MotorMotion::Absolute
                                                                            : ::motion::MotorMotion::Relative,
                                        .position = c.position[0],
                                        .velocity = c.speed,
                                        .acceleration = c.acceleration,
                                        .deceleration = c.deceleration,
                                        .jerk = c.jerk,
                                        .timeout = c.timeout,
                                        .latch_velocity = c.latch,
                                        .buffer = static_cast<::motion::MotorBuffer>(c.buffer) });
                    break;
                case AxisRelative:
                case AxisAbsolute:
                case AxisHome:
                case AxisJog: {
                    motion::Robot::ExecutionRequest r;
                    r.family = ::motion::Family::Axis;
                    r.axes = std::uint8_t(1U << c.axis);
                    r.kind = c.kind == AxisHome  ? motion::Robot::ExecutionRequest::Kind::Home
                             : c.kind == AxisJog ? motion::Robot::ExecutionRequest::Kind::Jog
                                                 : motion::Robot::ExecutionRequest::Kind::Finite;
                    const double unit = c.axis == 2 ? .001 : RAD;
                    r.relative = c.kind == AxisRelative;
                    r.distance = c.position[0] * unit;
                    r.speed = c.speed * unit;
                    r.acceleration = c.acceleration * unit;
                    r.deceleration = c.deceleration * unit;
                    r.jerk = c.jerk * unit;
                    r.timeout = c.timeout;
                    r.duration = c.duration;
                    r.continuous = c.continuous;
                    reply.job = m_robot.submit(r);
                    break;
                }
                case JointRelative:
                case JointAbsolute:
                case PointRelative:
                case PointAbsolute: {
                    motion::Robot::ExecutionRequest r;
                    r.relative = c.kind == JointRelative || c.kind == PointRelative;
                    r.move.kind = c.kind == JointRelative || c.kind == JointAbsolute
                                    ? motion::MoveKind::Joints
                                    : motion::MoveKind::Point;
                    r.move.joints = joints(c.position);
                    r.move.target = point(c.position);
                    r.timeout = c.timeout;
                    r.dynamics = { c.speed, c.acceleration, c.deceleration, c.jerk };
                    reply.job = m_robot.submit(r);
                    break;
                }
                case PointJog: {
                    motion::Robot::ExecutionRequest r;
                    r.kind = motion::Robot::ExecutionRequest::Kind::CartesianJog;
                    r.direction = c.position;
                    r.dynamics = { c.speed, c.acceleration, c.deceleration, c.jerk };
                    r.duration = c.duration;
                    r.continuous = c.continuous;
                    r.timeout = c.timeout;
                    reply.job = m_robot.submit(r);
                    break;
                }
                case RobotHome:
                    reply.job = m_robot.home();
                    break;
                case PreviewJoint:
                case PreviewPoint:
                case PreviewLine: {
                    motion::Robot::PreviewRequest r;
                    r.move.kind = c.kind == PreviewJoint   ? motion::MoveKind::Joints
                                  : c.kind == PreviewPoint ? motion::MoveKind::Point
                                                           : motion::MoveKind::Linear;
                    r.move.joints = joints(c.position);
                    r.move.target = point(c.position);
                    r.move.speed = c.speed;
                    auto model = m_robot.presentationModel();
                    if (!model || !state.poseValid)
                        throw std::runtime_error("Preview needs configured, referenced starting state");
                    r.start = model->joints(state.commanded);
                    const auto result = m_robot.preview(r);
                    if (!result)
                        throw std::runtime_error(std::string(tr::name(result.error())));
                    reply.job = *result;
                    break;
                }
                case CancelPreview:
                    if (m_robot.cancel(c.job) != tr::Error::None)
                        throw std::runtime_error("Preview cancellation rejected");
                    break;
                case MemoryUI:
                case MemoryIO:
                case MemoryUIStack:
                case MemoryIOStack: {
                    std::scoped_lock lock{ m_mutex };
                    if (m_memoryBusy)
                        throw std::runtime_error("Memory query pending");
                    m_memoryBusy = true;
                    m_memoryOperation = reply.operation;
                    m_memoryRequest = c.kind == MemoryIO        ? 1
                                      : c.kind == MemoryUIStack ? 2
                                      : c.kind == MemoryIOStack ? 3
                                                                : 0;
                    m_client.notify();
                    break;
                }
            }
            reply.accepted = true;
            std::snprintf(reply.message.data(),
                          reply.message.size(),
                          reply.job ? "Submitted job #%llu; awaiting IO acceptance"
                                    : "Request submitted; awaiting completion",
                          (unsigned long long)reply.job);
        } catch (const std::exception& e) {
            text(reply.message, e.what());
        }
        return reply;
    }
    unsigned GuiBackend::jobs(unsigned offset, std::span<hmi::Job> output, bool previews)
    {
        if (previews) {
            const auto ids = m_robot.jobs();
            unsigned n{}, skip{};
            for (auto id : ids) {
                if (!id)
                    continue;
                if (skip++ < offset)
                    continue;
                if (n == output.size())
                    break;
                const auto j = m_robot.job(id);
                if (!j)
                    continue;
                auto& out = output[n++];
                out = {};
                out.id = id;
                out.preview = true;
                out.severity = j->state == motion::Robot::JobState::Rejected ? hmi::Severity::Warning
                                                                             : hmi::Severity::Info;
                out.state = unsigned(j->state);
                out.terminal = j->state >= motion::Robot::JobState::Preview;
                out.seconds = j->plan.duration;
                text(out.status,
                     j->state == motion::Robot::JobState::Preview    ? "Preview ready"
                     : j->state == motion::Robot::JobState::Rejected ? "Rejected"
                                                                     : "Planning");
                std::snprintf(out.detail.data(),
                              out.detail.size(),
                              "%s; approx %.4f mm; quantization %.4f mm",
                              tr::name(j->error).data(),
                              j->plan.path_error_bound * 1000,
                              j->plan.quantization_bound * 1000);
                for (unsigned i = 0; i < 3; ++i)
                    out.endpoint[i] = j->plan.endpoint[i];
            }
            return n;
        }
        std::array<motion::Client::JobInfo, 4> jobs{};
        const auto count =
          m_client.copyJobs(offset, std::span(jobs).first(std::min(output.size(), jobs.size())));
        for (unsigned i = 0; i < count; ++i) {
            const auto& j = jobs[i];
            auto& out = output[i];
            out = {};
            out.id = j.id;
            out.busy = j.busy_job;
            out.state = unsigned(j.state);
            out.severity =
              j.state == motion::Client::JobState::Faulted || j.state == motion::Client::JobState::Unknown
                ? hmi::Severity::Error
              : j.state == motion::Client::JobState::TimedOut || j.state == motion::Client::JobState::Rejected
                ? hmi::Severity::Warning
                : hmi::Severity::Info;
            out.axes = j.axes;
            out.family = unsigned(j.family);
            out.terminal = j.state >= motion::Client::JobState::Completed;
            out.seconds = j.planned_seconds;
            text(out.status, jobNames[unsigned(j.state)]);
            if (j.busy_job)
                std::snprintf(out.detail.data(),
                              out.detail.size(),
                              "Busy owner #%llu; stop or wait",
                              (unsigned long long)j.busy_job);
            else
                text(out.detail, tr::name(j.error).data());
            const auto& requested = j.requested;
            char input[100]{};
            using Space = motion::Client::RequestSpace;
            const auto& v = requested.values;
            switch (requested.space) {
                case Space::Joints:
                    std::snprintf(input,
                                  sizeof(input),
                                  "%s %.3g / %.3g deg / %.3g mm",
                                  requested.relative ? "Delta" : "Target",
                                  v[0] / RAD,
                                  v[1] / RAD,
                                  v[2] * 1000);
                    break;
                case Space::Point:
                    std::snprintf(input,
                                  sizeof(input),
                                  "%s XYZ %.3g / %.3g / %.3g mm",
                                  requested.relative ? "Delta" : "Target",
                                  v[0] * 1000,
                                  v[1] * 1000,
                                  v[2] * 1000);
                    break;
                case Space::Rotary:
                case Space::Linear:
                case Space::Shaft:
                    std::snprintf(input,
                                  sizeof(input),
                                  "%s %.4g %s",
                                  requested.relative ? "Delta" : "Target",
                                  v[0] * (requested.space == Space::Rotary   ? 1 / RAD
                                          : requested.space == Space::Linear ? 1000
                                                                             : 1),
                                  requested.space == Space::Linear ? "mm" : "deg");
                    break;
                case Space::Home:
                    text(out.detail, "Reference procedure");
                    break;
                case Space::Jog:
                    text(out.detail,
                         j.family == ::motion::Family::Robot
                           ? "Cartesian jog; stops at certified path boundary or on cancellation"
                           : "Supervised jog; endpoint is soft limit, not final resting position");
                    break;
                default:
                    break;
            }
            if (input[0]) {
                const auto reason = out.detail;
                std::snprintf(out.detail.data(), out.detail.size(), "%s · %.85s", input, reason.data());
            }
            if (j.family == ::motion::Family::Motor && j.state == motion::Client::JobState::Rejected &&
                j.error == tr::Error::None)
                text(out.detail, "Raw motion rejected: inspect readiness, reference and driver state");
            for (unsigned k = 0; k < 3; ++k)
                out.endpoint[k] = j.endpoint[k];
        }
        return count;
    }
    hmi::Outcome GuiBackend::hold(const hmi::Command& c, int x, int y, int w, int h)
    {
        release();
        std::uint64_t gesture{};
        {
            std::scoped_lock lock{ m_mutex };
            m_holding = true;
            gesture = ++m_gesture;
            m_x = x;
            m_y = y;
            m_width = w;
            m_height = h;
            m_inputDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{ 200 };
        }
        return submitHeld(c, gesture);
    }
    hmi::Outcome GuiBackend::submitHeld(const hmi::Command& c, std::uint64_t gesture)
    {
        auto result = submit(c);
        bool abandoned{};
        {
            std::scoped_lock lock{ m_mutex };
            const auto now = std::chrono::steady_clock::now();
            abandoned = !m_holding || gesture != m_gesture || now >= m_inputDeadline;
            if (!abandoned && result.accepted) {
                m_holdJob = result.job;
                m_client.superviseJog(result.job);
            }
            else if (gesture == m_gesture)
                m_holding = false;
        }
        if (abandoned && result.job)
            m_client.cancelAsync(result.job);
        return result;
    }
    void GuiBackend::input(const hmi::Input& input)
    {
        std::scoped_lock lock{ m_mutex };
        if (!m_holding)
            return;
        const auto now = std::chrono::steady_clock::now();
        const bool stop = now >= m_inputDeadline || !input.valid || !input.down || input.x < m_x ||
                          input.y < m_y || input.x >= m_x + m_width || input.y >= m_y + m_height ||
                          now - input.sampled > std::chrono::milliseconds{ 100 };
        if (stop) {
            m_holding = false;
            const auto job = m_holdJob;
            m_holdJob = 0;
            ++m_gesture;
            m_client.releaseJogInput();
            if (job)
                m_client.cancelAsync(job);
        }
        else {
            m_inputDeadline = now + std::chrono::milliseconds{ 200 };
            if (m_holdJob)
                m_client.renewJogInput(m_holdJob);
        }
    }
    void GuiBackend::release()
    {
        {
            std::scoped_lock lock{ m_mutex };
            m_holding = false;
            const auto job = m_holdJob;
            m_holdJob = 0;
            ++m_gesture;
            m_client.releaseJogInput();
            if (job)
                m_client.cancelAsync(job);
        }
    }
}
