#include "Services.hpp"
#include "common/icc/catalog/services.hpp"
#include "services/Text.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace ui
{
    namespace hmi = services;
    namespace tr = ::motion::trajectory;
    namespace
    {
        constexpr double MILLIMETRES_PER_METRE{ 1000 };
        constexpr unsigned MEMORY_IO{ 1U }, MEMORY_STACK{ 2U };
        constexpr std::uint8_t ALL_AXES{ 0b111 };
        constexpr auto TELEMETRY_STALE{ std::chrono::milliseconds{ 500 } };
        constexpr auto MEMORY_TIMEOUT{ std::chrono::seconds{ 3 } };
        constexpr unsigned LAST_BUFFER_MODE{ 5 }, JOB_PAGE_SIZE{ 4 };
        constexpr unsigned PERCENT{ 100 }, REQUEST_TEXT_BYTES{ 100 };
        constexpr double RAD{ std::numbers::pi / 180 };
        constexpr std::array JOB_NAMES{ "Submitting", "Planning", "Preparing", "Accepted",
                                        "Running",    "Stopping", "Completed", "Stopped",
                                        "Timed out",  "Rejected", "Faulted",   "Unknown" };
        template<std::size_t N>
        void text(std::array<char, N>& out, std::string_view value)
        {
            static_assert(N > 0);
            const auto count{ std::min(value.size(), N - 1) };
            std::copy_n(value.begin(), count, out.begin());
            out[count] = '\0';
        }
        motion::JointPosition joints(const std::array<double, 3>& p)
        {
            return { pnm::units::Angle::create(p[0] * RAD),
                     pnm::units::Angle::create(p[1] * RAD),
                     pnm::units::Distance::create(p[2] / MILLIMETRES_PER_METRE) };
        }
        motion::CartesianPosition point(const std::array<double, 3>& p)
        {
            return { pnm::units::Distance::create(p[0] / MILLIMETRES_PER_METRE),
                     pnm::units::Distance::create(p[1] / MILLIMETRES_PER_METRE),
                     pnm::units::Distance::create(p[2] / MILLIMETRES_PER_METRE) };
        }
    }
    namespace
    {
        void validate_command(const hmi::Command& c, const hmi::Snapshot& state)
        {
            const bool local{ c.kind == hmi::Kind::MemoryUI || c.kind == hmi::Kind::MemoryUIStack ||
                              c.kind == hmi::Kind::CancelPreview || c.kind == hmi::Kind::Brightness ||
                              c.kind == hmi::Kind::AcknowledgeMessages };
            const bool stop{ c.kind == hmi::Kind::Stop || c.kind == hmi::Kind::Abort ||
                             c.kind == hmi::Kind::Disable || c.kind == hmi::Kind::Cancel ||
                             c.kind == hmi::Kind::AxisStop || c.kind == hmi::Kind::MotorAbort };
            if (!local && (!state.connected || state.failed)) {
                throw std::runtime_error("IO disconnected; stop unconfirmed; restart required");
            }
            if (!local && !stop && !state.fresh) {
                throw std::runtime_error("IO telemetry stale; command not submitted");
            }
            if (c.axis >= 3 || c.buffer > LAST_BUFFER_MODE) {
                throw std::invalid_argument("Invalid axis or buffer mode");
            }
            for (double value : { c.position[0],
                                  c.position[1],
                                  c.position[2],
                                  c.speed,
                                  c.acceleration,
                                  c.deceleration,
                                  c.jerk,
                                  c.timeout,
                                  c.duration,
                                  c.latch }) {
                if (!std::isfinite(value)) {
                    throw std::invalid_argument("Nonfinite motion parameter");
                }
            }
        }
    }
    namespace
    {
        const char* motion_condition(const hmi::Snapshot& state, bool referenced, bool fault, bool busy)
        {
            if (fault) {
                return "Driver fault: disable and inspect diagnostics";
            }
            if (!state.initialized) {
                return "Initialization required (does not enable)";
            }
            if (!state.enabled) {
                return "Disabled: initialize, then explicitly enable";
            }
            if (!state.configured) {
                return "Mechanical calibration required for axes / robot";
            }
            if (!referenced) {
                return "Unreferenced: home the required axes";
            }
            if (busy) {
                return "Motion active: Stop remains available";
            }
            return "Ready";
        }
        ::motion::MotorMotion raw_kind(hmi::Kind kind)
        {
            if (kind == hmi::Kind::MotorHome) {
                return ::motion::MotorMotion::Home;
            }
            if (kind == hmi::Kind::MotorAbsolute) {
                return ::motion::MotorMotion::Absolute;
            }
            return ::motion::MotorMotion::Relative;
        }
        motion::Robot::ExecutionRequest::Kind axis_kind(hmi::Kind kind)
        {
            using Kind = motion::Robot::ExecutionRequest::Kind;
            if (kind == hmi::Kind::AxisHome) {
                return Kind::Home;
            }
            if (kind == hmi::Kind::AxisJog) {
                return Kind::Jog;
            }
            return Kind::Finite;
        }
        motion::MoveKind preview_kind(hmi::Kind kind)
        {
            if (kind == hmi::Kind::PreviewJoint) {
                return motion::MoveKind::Joints;
            }
            if (kind == hmi::Kind::PreviewPoint) {
                return motion::MoveKind::Point;
            }
            return motion::MoveKind::Linear;
        }
        const char* preview_status(motion::Robot::JobState state)
        {
            if (state == motion::Robot::JobState::Preview) {
                return "Preview ready";
            }
            if (state == motion::Robot::JobState::Rejected) {
                return "Rejected";
            }
            return "Planning";
        }
        hmi::Severity job_severity(motion::Client::JobState state)
        {
            using enum motion::Client::JobState;
            switch (state) {
                case Faulted:
                case Unknown:
                    return hmi::Severity::Error;
                case TimedOut:
                case Rejected:
                    return hmi::Severity::Warning;
                default:
                    return hmi::Severity::Info;
            }
        }
        double request_units(motion::Client::RequestSpace space)
        {
            using enum motion::Client::RequestSpace;
            if (space == Rotary) {
                return 1 / RAD;
            }
            if (space == Linear) {
                return MILLIMETRES_PER_METRE;
            }
            return 1;
        }
    }
    Services::Services(pnm::msg::Bus& bus,
                           motion::Client& client,
                           motion::Robot& robot,
                           SystemState& system)
      : m_client{ client }
      , m_robot{ robot }
      , m_system{ system }
      , m_memory{ bus.service<^^icc::Services::io_memory>() }
      , m_storage{ reserveStorage() }
    {
#ifdef HAL_PLATFORM_LINUX
        m_storage->state.simulation = true;
#endif
        text(m_storage->state.condition, "Awaiting IO telemetry");
        m_client.setObserver([this] { update(); });
    }
    Services::~Services() { m_client.setObserver({}); }
    void Services::connection(icc::ConnectionStatus state, const char* detail)
    {
        std::scoped_lock lock{ m_mutex };
        m_storage->state.connected = state.state == icc::ConnectionState::Connected;
        if (state.state == icc::ConnectionState::Failed ||
            state.reason == icc::ConnectionReason::PeerStopped) {
            m_storage->state.failed = true;
            m_storage->state.fresh = false;
            for (auto& operation : m_storage->operations) {
                if (!operation.pending) continue;
                operation.pending = false;
                operation.value.phase = hmi::Outcome::Phase::Unknown;
                operation.value.severity = hmi::Severity::Error;
                operation.value.sequence = ++m_outcomeSequence;
                text(operation.value.message, "ICC lost; hardware outcome unknown; no automatic retry");
            }
            hmi::format(m_storage->state.condition,
                        "ICC failure {}: restart required; physical stop unconfirmed",
                        unsigned(state.reason));
            if (detail && *detail) {
                hmi::format(m_storage->state.condition,
                            "ICC failure {}: {:.96}; restart required",
                            unsigned(state.reason),
                            detail);
            }
        }
    }
    void Services::read(hmi::Snapshot& result)
    {
        std::scoped_lock lock{ m_mutex };
        result = m_storage->state;
        const auto now{ std::chrono::steady_clock::now().time_since_epoch() };
        const auto us{ std::chrono::duration_cast<std::chrono::microseconds>(now).count() };
        if (result.received_us) {
            result.age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::microseconds{ us - result.received_us })
                              .count();
        }
        if (std::cmp_greater(result.age_ms, TELEMETRY_STALE.count())) {
            result.fresh = false;
            if (!result.failed) {
                text(result.condition, "Telemetry stale: pose frozen; motion unavailable");
            }
        }
    }
    void Services::outcome(std::uint64_t id, bool ok, const char* message, std::uint64_t job)
    {
        hmi::Outcome value{};
        value.operation = id;
        value.accepted = ok;
        value.severity = ok ? hmi::Severity::Info : hmi::Severity::Error;
        value.job = job;
        value.phase = ok ? hmi::Outcome::Phase::Completed : hmi::Outcome::Phase::Rejected;
        hmi::format(value.message, "{} (#{}){}", message, (job ? job : id), ok ? "" : "; no automatic retry");
        publishOutcome(value, false);
    }

    namespace
    {
        struct MotorSummary
        {
            bool referenced{ true }, feedback{ true }, fault{}, busy{};
            tr::Positions commanded{}, positions{};
        };
        MotorSummary present_motors(hmi::Snapshot& next,
                                    const ::motion::MotorSnapshot& state,
                                    const std::optional<motion::RobotModel>& model)
        {
            MotorSummary summary{};
            for (unsigned i{ 0 }; i < 3; ++i) {
                const auto& motor{ state.motors[i] };
                auto& a{ next.axes[i] };
                a.shaft = motor.position;
                a.rpm = motor.velocity;
                a.shaft_feedback = motor.actual_position;
                a.reference = motor.switch_active;
                a.referenced = motor.referenced;
                a.valid = motor.position_valid;
                a.velocity_valid = motor.velocity_valid;
                a.ready = motor.driver_ready;
                a.fault = motor.fault_active || motor.fault_latched;
                a.fault_code = motor.fault;
                a.warning = motor.warning;
                a.encoder = motor.feedback == ::motion::MotorFeedback::Encoder;
                a.owner = state.owners[i];
                next.joint_for_motor[i] = i;
                a.acceleration = motor.acceleration;
                a.deceleration = motor.deceleration;
                a.jerk = motor.jerk;
                text(a.driver, motor.driver);
                summary.referenced &= a.referenced;
                summary.feedback &= a.valid;
                summary.fault |= a.fault;
                summary.busy |= a.owner != 0;
                if (model) {
                    summary.commanded[i] = motor.position * RAD / model->hardware().step_radians[i];
                    summary.positions[i] = motor.actual_position * RAD / model->hardware().step_radians[i];
                }
            }
            return summary;
        }
        void present_joints(hmi::Snapshot& next, const motion::RobotModel& model, const MotorSummary& summary)
        {
            const auto q{ model.toJoints(summary.commanded) };
            next.commanded = motion::RobotModel::joints(q);
            const auto aq{ model.toJoints(summary.positions) };
            next.feedback = motion::RobotModel::joints(aq);
            next.pose_valid = summary.referenced;
            next.feedback_valid = summary.referenced && summary.feedback;
            const auto limits{ model.jointLimits() };
            // Axis presentation indexes joints, remapping the corresponding motor metadata.
            const auto motor_axes{ next.axes };
            for (unsigned i{ 0 }; i < 3; ++i) {
                const auto motor{ model.configuration().motor_for_joint[i] };
                auto& a{ next.axes[i] };
                a = motor_axes[motor];
                next.joint_for_motor[motor] = i;
                const double unit{ i == 2 ? MILLIMETRES_PER_METRE : 1 / RAD };
                a.commanded = next.commanded[i] * unit;
                a.feedback = next.feedback[i] * unit;
                // RPM -> motor radians/s -> signed pulses/s -> joint units/s.
                a.velocity = a.rpm * (2 * std::numbers::pi / 60) /
                    model.hardware().step_radians[motor] / model.stepsPerJointUnit()[i] * unit;
                a.minimum = limits[i].minimum * unit;
                a.maximum = limits[i].maximum * unit;
                a.max_speed = limits[i].velocity * unit;
            }
            if (model.hasKinematics()) {
                if (const auto fk{ model.kinematics().forward(q) }) {
                    next.tcp = { fk->position.x.get() * MILLIMETRES_PER_METRE,
                                 fk->position.y.get() * MILLIMETRES_PER_METRE,
                                 fk->position.z.get() * MILLIMETRES_PER_METRE };
                    next.yaw = fk->yaw.get() / RAD;
                }
            }
            if (model.hasKinematics()) {
                if (const auto fk{ model.kinematics().forward(aq) }) {
                    next.feedback_tcp = { fk->position.x.get() * MILLIMETRES_PER_METRE,
                                          fk->position.y.get() * MILLIMETRES_PER_METRE,
                                          fk->position.z.get() * MILLIMETRES_PER_METRE };
                    next.feedback_yaw = fk->yaw.get() / RAD;
                }
            }
            const auto& config{ model.configuration() };
            hmi::format(next.details,
                        "{} configuration\nMotor mapping {} / {} / {}\nLimits and transmission: read "
                        "only\nM1 encoder; M2/M3 INDEX are phase estimates",
                        config.purpose == motion::RobotConfiguration::Purpose::Simulation
                          ? "Native simulation"
                          : "Verified physical",
                        config.motor_for_joint[0] + 1,
                        config.motor_for_joint[1] + 1,
                        config.motor_for_joint[2] + 1);
        }
    }
    void Services::update()
    {
        hmi::Snapshot next{};
        std::array<unsigned, 3> previousFaults{};
        {
            std::scoped_lock lock{ m_mutex };
            next = m_storage->state;
            for (unsigned i{}; i < 3; ++i) previousFaults[i] = next.axes[i].fault ? next.axes[i].fault_code + 1U : 0U;
        }
        const auto system{ m_system.snapshot() };
        next.ui_state = unsigned(system.ui);
        next.io_state = unsigned(system.io.value_or(app::State::Starting));
        next.combined_state = unsigned(system.combined);
        next.io_state_fresh = system.io_fresh;
        try {
            const auto state{ m_client.state() };
            next.fresh = true;
            next.sequence = state.sequence;
            next.boot = state.boot;
            next.configuration = state.configuration;
            next.received_us =
              std::chrono::duration_cast<std::chrono::microseconds>(m_client.receivedAt().time_since_epoch())
                .count();
            next.enabled = state.enabled;
            next.initialized = state.ready;
            next.family = unsigned(state.family);
            m_robot.requestPresentationModel(state.boot, state.configuration);
            const auto model{ m_robot.presentationModel() };
            next.configured = bool(model);
            next.robot_configured = model && model->hasKinematics();
            next.pose_valid = false;
            next.feedback_valid = false;
            const auto summary{ present_motors(next, state, model) };
            if (model) {
                present_joints(next, *model, summary);
            }
            const auto* condition{ motion_condition(next, summary.referenced, summary.fault, summary.busy) };
            if (!next.failed) {
                text(next.condition, condition);
            }
        } catch (const std::exception& e) {
            next.fresh = false;
            if (!next.failed) {
                text(next.condition, e.what());
            }
        }
        {
            std::scoped_lock lock{ m_mutex };
            // Connection/outcome can change concurrently while we collect values.
            next.connected = m_storage->state.connected;
            next.failed = m_storage->state.failed;
            next.outcome = m_storage->state.outcome;
            if (m_storage->state.failed) {
                next.fresh = false;
                next.condition = m_storage->state.condition;
            }
            m_storage->state = next;
        }
        for (unsigned i{}; i < 3; ++i) {
            if (next.axes[i].fault && previousFaults[i] != next.axes[i].fault_code + 1U) {
                services::Text<160> text{};
                services::format(text, "Axis {} driver fault {}; recovery requires disabled-state initialization", i + 1, next.axes[i].fault_code);
                message(text.data(), services::Severity::Error);
            }
        }
        updateMemory();
        m_robot.expireRemotePreviews();
        notifySubscribers();
    }
    void Services::updateMemory()
    {
        std::optional<unsigned> memory{};
        {
            std::scoped_lock lock{ m_mutex };
            if (!m_memoryCall) {
                memory = std::exchange(m_memoryRequest, {});
            }
        }
        if (memory) {
            if (*memory & MEMORY_IO) {
                m_memoryCall.emplace(m_memory.request(
                  (*memory & MEMORY_STACK) ? diagnostics::MemoryQuery::Stack : diagnostics::MemoryQuery::Heap,
                  MEMORY_TIMEOUT));
                m_memoryReady = pnm::msg::watch(*m_memoryCall, [this] { m_client.notify(); });
            }
            else {
                const auto data{ diagnostics::collect_memory((*memory & MEMORY_STACK)
                                                               ? diagnostics::MemoryQuery::Stack
                                                               : diagnostics::MemoryQuery::Heap) };
                outcome(m_memoryOperation, true, "UI diagnostics updated");
                std::scoped_lock lock{ m_mutex };
                m_storage->memory = data;
                m_memoryTime = std::chrono::steady_clock::now();
                m_memoryAvailable = true;
                m_memoryIO = false;
                m_memoryError = {};
                m_memoryBusy = false;
            }
        }
        if (m_memoryCall && m_memoryCall->ready()) {
            auto data{ m_memoryCall->get() };
            m_memoryReady = {};
            m_memoryCall.reset();
            outcome(
              m_memoryOperation, bool(data), data ? "IO diagnostics updated" : "IO diagnostics unavailable");
            std::scoped_lock lock{ m_mutex };
            m_memoryIO = true;
            m_memoryAvailable = bool(data);
            m_memoryError = {};
            if (data) {
                m_storage->memory = *data;
                m_memoryTime = std::chrono::steady_clock::now();
            }
            else {
                hmi::format(m_memoryError, "IO request failed ({})", unsigned(data.error()));
            }
            m_memoryBusy = false;
        }
    }
    void Services::diagnostics(unsigned offset, hmi::DiagnosticPage& out)
    {
        std::scoped_lock lock{ m_mutex };
        out = {};
        out.busy = m_memoryBusy;
        out.available = m_memoryAvailable;
        out.io = m_memoryIO;
        out.error = m_memoryError;
        if (!out.available) {
            return;
        }
        out.age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                           m_memoryTime)
                       .count();
        out.stacks = m_storage->memory.query == diagnostics::MemoryQuery::Stack;
        out.shared = m_storage->memory.shared_runtime;
        if (out.stacks) {
            const auto& stacks{ m_storage->memory.stack };
            out.total = std::min<unsigned>(stacks.count, stacks.threads.size());
            out.omitted = stacks.total > out.total ? stacks.total - out.total : 0;
            for (unsigned i{ offset }; i < out.total && out.count < out.rows.size(); ++i) {
                auto& row{ out.rows[out.count++] };
                const auto& t{ stacks.threads[i] };
                text(row.name,
                     std::string_view{
                       t.name.data(),
                       static_cast<std::size_t>(std::ranges::find(t.name, '\0') - t.name.begin()) });
                row.values = { t.prio, t.stack_size, t.current_stack_usage, t.peak_stack_usage };
            }
        }
        else {
            const auto& h{ m_storage->memory.heap };
            constexpr std::array names{ "Capacity [B]",   "Reserved [B]",   "Used [B]",
                                        "Available [B]",  "Releasable [B]", "Free blocks",
                                        "Mapped regions", "Mapped [B]",     "Usage [%]" };
            const auto capacity{ h.capacity ? h.capacity : h.reserved };
            const std::array<std::uint64_t, names.size()> values{
                h.capacity,       h.reserved,     h.used,
                h.available,      h.releasable,   h.free_blocks,
                h.mapped_regions, h.mapped_bytes, capacity ? h.used * PERCENT / capacity : 0
            };
            out.total = values.size();
            out.allocator = unsigned(h.allocator);
            out.capacity = capacity;
            out.used = h.used;
            for (unsigned i{ offset }; i < out.total && out.count < out.rows.size(); ++i) {
                auto& row{ out.rows[out.count++] };
                text(row.name, names[i]);
                row.values[0] = values[i];
            }
        }
    }
    void Services::submitControl(std::uint64_t id, const ::motion::MotorRequest& request, std::uint64_t source)
    {
        if (!m_client.requestAsync(request, [this, id](auto result) {
            if (!result) {
                services::Outcome value{};
                value.operation = id;
                value.phase = services::Outcome::Phase::Unknown;
                value.severity = services::Severity::Error;
                text(value.message, "Control transport failed; hardware outcome unknown; no automatic retry");
                publishOutcome(value, false);
                return;
            }
            const char* message{ result->ok ? "Control acknowledged" : "Control rejected" };
            if (!result->message.empty()) {
                message = result->message.c_str();
            }
            outcome(id, result->ok, message);
        }, source)) {
            throw std::runtime_error("Control queue full or disconnected; retry after acknowledgement");
        }
    }
    std::uint64_t Services::submitRaw(const hmi::Command& c, const hmi::Snapshot& state, std::uint64_t source)
    {
        using enum hmi::Kind;
        if (!c.confirmed && c.buffer == 0) {
            bool occupied{ state.motor(c.axis).owner != 0 };
            std::array<motion::Client::JobInfo, JOB_PAGE_SIZE> pending{};
            for (unsigned offset{ 0 };; offset += JOB_PAGE_SIZE) {
                const auto count{ m_client.copyJobs(offset, pending) };
                for (unsigned i{ 0 }; i < count; ++i) {
                    occupied |= pending[i].state < motion::Client::JobState::Completed &&
                                (pending[i].axes & (1U << c.axis));
                }
                if (count < JOB_PAGE_SIZE) {
                    break;
                }
            }
            if (occupied) {
                throw std::runtime_error("Active raw motion requires replacement confirmation");
            }
        }
        return m_client.submit({ .motor = static_cast<::motion::MotorId>(c.axis),
                                 .motion = raw_kind(c.kind),
                                 .position = c.position[0],
                                 .velocity = c.speed,
                                 .acceleration = c.acceleration,
                                 .deceleration = c.deceleration,
                                 .jerk = c.jerk,
                                 .timeout = c.timeout,
                                 .latch_velocity = c.latch,
                                 .buffer = static_cast<::motion::MotorBuffer>(c.buffer) }, {}, source);
    }
    std::uint64_t Services::submitAxis(const hmi::Command& c, std::uint64_t source)
    {
        using enum hmi::Kind;
        motion::Robot::ExecutionRequest r{};
        r.source = source;
        r.family = ::motion::Family::Axis;
        r.axes = std::uint8_t(1U << c.axis);
        r.kind = axis_kind(c.kind);
        const double unit{ c.axis == 2 ? 1 / MILLIMETRES_PER_METRE : RAD };
        r.relative = c.kind == AxisRelative;
        r.distance = c.position[0] * unit;
        r.speed = c.speed * unit;
        r.acceleration = c.acceleration * unit;
        r.deceleration = c.deceleration * unit;
        r.jerk = c.jerk * unit;
        r.timeout = c.timeout;
        r.duration = c.duration;
        r.continuous = c.continuous;
        return m_robot.submit(r);
    }
    std::uint64_t Services::submitRobot(const hmi::Command& c, std::uint64_t source)
    {
        using enum hmi::Kind;
        motion::Robot::ExecutionRequest r{};
        r.source = source;
        r.relative = c.kind == JointRelative || c.kind == PointRelative;
        r.move.kind = c.kind == JointRelative || c.kind == JointAbsolute ? motion::MoveKind::Joints
                                                                         : motion::MoveKind::Point;
        r.move.joints = joints(c.position);
        r.move.target = point(c.position);
        r.timeout = c.timeout;
        r.dynamics = {
            .speed = c.speed, .acceleration = c.acceleration, .deceleration = c.deceleration, .jerk = c.jerk
        };
        return m_robot.submit(r);
    }
    std::uint64_t Services::submitCartesianJog(const hmi::Command& c, std::uint64_t source)
    {
        using enum hmi::Kind;
        motion::Robot::ExecutionRequest r{};
        r.source = source;
        r.kind = motion::Robot::ExecutionRequest::Kind::CartesianJog;
        r.direction = c.position;
        r.dynamics = {
            .speed = c.speed, .acceleration = c.acceleration, .deceleration = c.deceleration, .jerk = c.jerk
        };
        r.duration = c.duration;
        r.continuous = c.continuous;
        r.timeout = c.timeout;
        return m_robot.submit(r);
    }
    std::uint64_t Services::submitPreview(const hmi::Command& c, const hmi::Snapshot& state, std::uint64_t source)
    {
        using enum hmi::Kind;
        motion::Robot::PreviewRequest r{};
        r.source = source;
        r.move.kind = preview_kind(c.kind);
        r.move.joints = joints(c.position);
        r.move.target = point(c.position);
        r.move.speed = c.speed;
        auto model{ m_robot.presentationModel() };
        if (!model || !state.pose_valid) {
            throw std::runtime_error("Preview needs configured, referenced starting state");
        }
        r.start = motion::RobotModel::joints(state.commanded);
        const auto result{ m_robot.preview(r) };
        if (!result) {
            throw std::runtime_error(std::string(tr::name(result.error())));
        }
        return *result;
    }
    void Services::requestMemory(const hmi::Command& c, std::uint64_t operation)
    {
        using enum hmi::Kind;
        std::scoped_lock lock{ m_mutex };
        if (m_memoryBusy) {
            throw std::runtime_error("Memory query pending");
        }
        m_memoryBusy = true;
        m_memoryOperation = operation;
        m_memoryRequest = (c.kind == MemoryIO || c.kind == MemoryIOStack ? MEMORY_IO : 0U) |
                          (c.kind == MemoryUIStack || c.kind == MemoryIOStack ? MEMORY_STACK : 0U);
        m_client.notify();
    }
    hmi::Outcome Services::submit(const hmi::Command& c, std::uint64_t source)
    {
        hmi::Outcome reply{};
        hmi::Snapshot state{};
        const bool urgent{ c.kind == hmi::Kind::Stop || c.kind == hmi::Kind::Abort ||
            c.kind == hmi::Kind::Disable || c.kind == hmi::Kind::Cancel ||
            c.kind == hmi::Kind::AxisStop || c.kind == hmi::Kind::MotorAbort };
        reply.operation = reserveOperation(source, urgent);
        if (!reply.operation) {
            text(reply.message, "Frontend operation capacity exhausted; retry after completion");
            reply.phase = hmi::Outcome::Phase::Rejected;
            return reply;
        }
        read(state);
        try {
            if (source && !m_client.remoteAuthorized(source)) throw std::runtime_error("Remote command permission revoked or control generation expired");
            validate_command(c, state);
            using enum hmi::Kind;
            switch (c.kind) {
                case Brightness:
                    if (c.position[0] < 0 || c.position[0] > 100 || std::floor(c.position[0]) != c.position[0])
                        throw std::invalid_argument("Brightness must be an integer from 0 to 100 percent");
                    if (!m_brightness || !m_brightness(static_cast<unsigned>(c.position[0])))
                        throw std::runtime_error("Display unavailable or request queue full");
                    break;
                case AcknowledgeMessages:
                    acknowledgeMessages(c.job);
                    reply.phase = hmi::Outcome::Phase::Completed;
                    break;
                case Initialize:
                    submitControl(reply.operation, { .operation = ::motion::MotorOperation::Initialize }, source);
                    break;
                case Enable:
                    submitControl(reply.operation, { .operation = ::motion::MotorOperation::Enable }, source);
                    break;
                case Disable:
                    m_client.stopAsync(ALL_AXES, true);
                    submitControl(reply.operation, { .operation = ::motion::MotorOperation::Disable }, source);
                    break;
                case Stop:
                    m_client.stopAsync();
                    break;
                case Abort:
                    m_client.stopAsync(ALL_AXES, true);
                    break;
                case Cancel:
                    m_client.cancelAsync(c.job);
                    break;
                case MotorAbort:
                    m_client.stopAsync(std::uint8_t(1U << c.axis), true);
                    break;
                case AxisStop: {
                    auto model{ m_robot.presentationModel() };
                    if (!model) {
                        throw std::runtime_error("Axis mapping unavailable; use global Stop");
                    }
                    m_client.stopAsync(std::uint8_t(1U << model->configuration().motor_for_joint[c.axis]));
                    break;
                }
                case MotorDefaults:
                case MotorSpeed:
                    submitControl(
                      reply.operation,
                      { .operation = c.kind == MotorDefaults ? ::motion::MotorOperation::Defaults
                                                             : ::motion::MotorOperation::Speed,
                        .motor = static_cast<::motion::MotorId>(c.axis),
                        .velocity = c.speed,
                        .acceleration = c.acceleration,
                        .deceleration = c.deceleration,
                        .jerk = c.jerk,
                        .dynamics_fields = std::uint8_t(c.kind == MotorDefaults ? ALL_AXES : 0) }, source);
                    break;
                case MotorRelative:
                case MotorAbsolute:
                case MotorHome:
                    reply.job = submitRaw(c, state, source);
                    break;
                case AxisRelative:
                case AxisAbsolute:
                case AxisHome:
                case AxisJog:
                    reply.job = submitAxis(c, source);
                    break;
                case JointRelative:
                case JointAbsolute:
                case PointRelative:
                case PointAbsolute:
                    reply.job = submitRobot(c, source);
                    break;
                case PointJog:
                    reply.job = submitCartesianJog(c, source);
                    break;
                case RobotHome: {
                    motion::Robot::ExecutionRequest request{};
                    request.source = source;
                    request.kind = motion::Robot::ExecutionRequest::Kind::Home;
                    reply.job = m_robot.submit(request);
                    break;
                }
                case PreviewJoint:
                case PreviewPoint:
                case PreviewLine:
                    reply.job = submitPreview(c, state, source);
                    break;
                case CancelPreview:
                    if (m_robot.cancel(c.job) != tr::Error::None) {
                        throw std::runtime_error("Preview cancellation rejected");
                    }
                    break;
                case MemoryUI:
                case MemoryIO:
                case MemoryUIStack:
                case MemoryIOStack:
                    requestMemory(c, reply.operation);
                    break;
            }
            reply.accepted = true;
            if (reply.job) {
                hmi::format(reply.message, "Submitted job #{}; awaiting IO acceptance", reply.job);
            }
            else {
                text(reply.message, "Request submitted; awaiting completion");
            }
        } catch (const std::exception& e) {
            text(reply.message, e.what());
        }
        if (!reply.accepted) {reply.phase = hmi::Outcome::Phase::Rejected; reply.severity = hmi::Severity::Warning;}
        const bool pending{ reply.accepted && (c.kind == hmi::Kind::Initialize || c.kind == hmi::Kind::Enable ||
            c.kind == hmi::Kind::Disable || c.kind == hmi::Kind::MotorDefaults || c.kind == hmi::Kind::MotorSpeed ||
            c.kind == hmi::Kind::MemoryUI || c.kind == hmi::Kind::MemoryIO ||
            c.kind == hmi::Kind::MemoryUIStack || c.kind == hmi::Kind::MemoryIOStack) };
        publishOutcome(reply, pending);
        return reply;
    }
    unsigned Services::previewJobs(unsigned offset, std::span<hmi::Job> output)
    {
        const auto ids{ m_robot.jobs() };
        unsigned n{};
        unsigned skip{};
        for (auto id : ids) {
            if (!id) {
                continue;
            }
            if (skip++ < offset) {
                continue;
            }
            if (n == output.size()) {
                break;
            }
            const auto j{ m_robot.job(id) };
            if (!j) {
                continue;
            }
            auto& out{ output[n++] };
            out = {};
            out.id = id;
            out.preview = true;
            out.source = j->source;
            out.severity =
              j->state == motion::Robot::JobState::Rejected ? hmi::Severity::Warning : hmi::Severity::Info;
            out.state = unsigned(j->state);
            out.terminal = j->state >= motion::Robot::JobState::Preview;
            out.seconds = j->plan.duration;
            text(out.status, preview_status(j->state));
            const auto reason{ tr::name(j->error) };
            hmi::format(out.detail,
                        "{}; approx {:.4f} mm; quantization {:.4f} mm",
                        reason,
                        j->plan.path_error_bound * MILLIMETRES_PER_METRE,
                        j->plan.quantization_bound * MILLIMETRES_PER_METRE);
            for (unsigned i{ 0 }; i < 3; ++i) {
                out.endpoint[i] = static_cast<double>(j->plan.endpoint[i]);
            }
        }
        return n;
    }
    namespace
    {
        void describe_request(const motion::Client::JobInfo& j, hmi::Job& out)
        {
            const auto& requested{ j.requested };
            hmi::Text<REQUEST_TEXT_BYTES> input{};
            using Space = motion::Client::RequestSpace;
            const auto& v{ requested.values };
            switch (requested.space) {
                case Space::Joints:
                    hmi::format(input,
                                "{} {:.3g} / {:.3g} deg / {:.3g} mm",
                                requested.relative ? "Delta" : "Target",
                                v[0] / RAD,
                                v[1] / RAD,
                                v[2] * MILLIMETRES_PER_METRE);
                    break;
                case Space::Point:
                    hmi::format(input,
                                "{} XYZ {:.3g} / {:.3g} / {:.3g} mm",
                                requested.relative ? "Delta" : "Target",
                                v[0] * MILLIMETRES_PER_METRE,
                                v[1] * MILLIMETRES_PER_METRE,
                                v[2] * MILLIMETRES_PER_METRE);
                    break;
                case Space::Rotary:
                case Space::Linear:
                case Space::Shaft:
                    hmi::format(input,
                                "{} {:.4g} {}",
                                requested.relative ? "Delta" : "Target",
                                v[0] * request_units(requested.space),
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
                const auto reason{ out.detail };
                hmi::format(out.detail, "{} · {:.85}", input.data(), reason.data());
            }
        }
        void present_job(const motion::Client::JobInfo& j, hmi::Job& out)
        {
            out = {};
            out.id = j.id; out.source = j.source;
            out.busy = j.busy_job;
            out.state = unsigned(j.state);
            out.severity = job_severity(j.state);
            out.axes = j.axes;
            out.family = unsigned(j.family);
            out.terminal = j.state >= motion::Client::JobState::Completed;
            out.seconds = j.planned_seconds;
            text(out.status, JOB_NAMES[unsigned(j.state)]);
            if (j.busy_job) {
                hmi::format(out.detail, "Busy owner #{}; stop or wait", j.busy_job);
            }
            else {
                text(out.detail, tr::name(j.error));
            }
            describe_request(j, out);
            if (j.family == ::motion::Family::Motor && j.state == motion::Client::JobState::Rejected &&
                j.error == tr::Error::None) {
                text(out.detail, "Raw motion rejected: inspect readiness, reference and driver state");
            }
            for (unsigned k{ 0 }; k < 3; ++k) {
                out.endpoint[k] = static_cast<double>(j.endpoint[k]);
            }
        }
    }
    unsigned Services::jobs(unsigned offset, std::span<hmi::Job> output, bool previews)
    {
        if (previews) {
            return previewJobs(offset, output);
        }
        std::array<motion::Client::JobInfo, JOB_PAGE_SIZE> jobs{};
        const auto count{ m_client.copyJobs(offset,
                                            std::span(jobs).first(std::min(output.size(), jobs.size()))) };
        for (unsigned i{ 0 }; i < count; ++i) {
            const auto& j{ jobs[i] };
            auto& out{ output[i] };
            present_job(j, out);
        }
        return count;
    }
    services::Configuration Services::configuration() const
    {
        services::Configuration out{};
        const auto model{m_robot.presentationModel()};
        if (!model) return out;
        const auto& c{model->configuration()};
        const auto& hw{model->hardware()};
        const auto limits{model->jointLimits()};
        const auto mm = [](const motion::CartesianPosition& p) {
            return std::array<double,3>{p.x.get()*1000, p.y.get()*1000, p.z.get()*1000};
        };
        out.available = true; out.calibrated = c.calibrated;
        out.simulation = c.purpose == motion::RobotConfiguration::Purpose::Simulation;
        out.kinematics = model->hasKinematics(); out.identity = model->fingerprint(); out.hardware = hw.hardware_fingerprint;
        for (unsigned i{}; i < 3; ++i) {
            const double unit{i == 2 ? 1000. : 1/RAD};
            out.motor_for_axis[i] = c.motor_for_joint[i] + 1;
            out.home_order[i] = c.homing_order[i] + 1;
            out.reduction[i] = i == 2 ? c.linear.motor_revolutions_per_output_revolution : c.rotary[i].motor_revolutions_per_axis_revolution;
            out.opposite[i] = (i == 2 ? c.linear.direction : c.rotary[i].direction) == motion::AxisDirection::OppositeToMotor;
            out.reference[i] = (i == 2 ? c.linear.axis_reference.get() : c.rotary[i].axis_reference.get()) * unit;
            out.minimum[i] = limits[i].minimum * unit; out.maximum[i] = limits[i].maximum * unit;
            out.velocity[i] = limits[i].velocity * unit; out.acceleration[i] = limits[i].acceleration * unit;
            out.deceleration[i] = limits[i].deceleration * unit; out.jerk[i] = limits[i].jerk * unit;
            out.motor_step_degrees[i] = hw.step_radians[i] / RAD;
            out.home_configured[i] = c.homing[i].configured; out.home_positive[i] = c.homing[i].positive;
            out.home_seek_rpm[i] = c.homing[i].seek.get() * 60 / (2*std::numbers::pi);
            out.home_latch_rpm[i] = c.homing[i].latch.get() * 60 / (2*std::numbers::pi);
            out.home_timeout_s[i] = c.homing[i].timeout.get();
            if (c.neutral_position) {out.neutral_available = true; out.neutral[i] = motion::RobotModel::joints(*c.neutral_position)[i] * unit;}
        }
        out.links_mm = {c.geometry.first_arm_length.get()*1000, c.geometry.second_arm_length.get()*1000};
        out.screw_lead_mm = c.linear.travel_per_output_revolution.get()*1000;
        out.base_mm = mm(c.geometry.base_origin); out.tool_mm = mm(c.geometry.tool_offset); out.work_mm = mm(c.work.origin);
        out.workspace_min_mm = mm(c.workspace_minimum); out.workspace_max_mm = mm(c.workspace_maximum);
        out.base_yaw_deg = c.geometry.base_yaw.get()/RAD; out.tool_yaw_deg = c.geometry.tool_yaw.get()/RAD;
        out.work_yaw_deg = c.work.yaw.get()/RAD; out.approximation_mm = c.path_tolerance.get()*1000;
        return out;
    }

    unsigned Services::detailedJobs(unsigned offset, std::span<services::DetailedJob> output, bool previews)
    {
        const auto limit{std::min<unsigned>(output.size(), JOB_PAGE_SIZE)};
        if (previews) {
            std::array<services::Job, JOB_PAGE_SIZE> rows{};
            const auto count{previewJobs(offset, std::span(rows).first(limit))};
            for (unsigned i{}; i < count; ++i) {output[i] = {}; static_cast<services::Job&>(output[i]) = rows[i];}
            return count;
        }
        std::array<motion::Client::JobInfo, JOB_PAGE_SIZE> rows{};
        const auto count{m_client.copyJobs(offset, std::span(rows).first(limit))};
        for (unsigned i{}; i < count; ++i) {
            auto& out{output[i]}; out = {};
            const auto& job{rows[i]};
            present_job(job, out);
            out.relative = job.requested.relative; out.requested = job.requested.values;
            using enum motion::Client::RequestSpace;
            switch (job.requested.space) {
                case Shaft: text(out.requested_space,"Shaft"); text(out.requested_units,"deg"); break;
                case Rotary: text(out.requested_space,"Rotary"); text(out.requested_units,"deg"); out.requested[0] /= RAD; break;
                case Linear: text(out.requested_space,"Linear"); text(out.requested_units,"mm"); out.requested[0] *= 1000; break;
                case Joints:
                    text(out.requested_space,"Joints"); text(out.requested_units,"deg,deg,mm");
                    out.requested[0] /= RAD; out.requested[1] /= RAD; out.requested[2] *= 1000; break;
                case Point:
                    text(out.requested_space,"Point"); text(out.requested_units,"mm,mm,mm");
                    for (auto& value : out.requested) value *= 1000;
                    break;
                case Home: text(out.requested_space,"Home"); break;
                case Jog:
                    text(out.requested_space, job.family == ::motion::Family::Robot ? "CartesianJog" : "AxisJog");
                    text(out.requested_units, job.family == ::motion::Family::Robot ? "direction vector" : job.axes == 4 ? "mm/s,s,held" : "deg/s,s,held");
                    if (job.family == ::motion::Family::Axis) out.requested[0] *= job.axes == 4 ? 1000. : 1/RAD;
                    break;
                case None: break;
            }
        }
        return count;
    }

}
