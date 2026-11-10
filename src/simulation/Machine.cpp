#include "Machine.hpp"
#include "hal/devices/util/Tmc2209Protocol.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>

namespace simulation
{
    namespace
    {
        namespace tmc = hal::device::util::tmc2209;
        constexpr std::int64_t ENCODER_COUNTS_PER_REVOLUTION{ 1600 };
        constexpr double DEGREES_PER_REVOLUTION{ 360.0 }, ENCODER_ROUNDING_TOLERANCE{ 1e-9 };
        constexpr double FULL_STEP_DEGREES{ 1.8 }, MILLIMETRES_PER_METRE{ 1000. }, MIDPOINT_DIVISOR{ 2. };
        constexpr unsigned ELECTRICAL_PHASE_STEPS{ 1024 }, INDEX_HIGH_STEPS{ 256 };
        constexpr unsigned ENCODER_A_PIN{ pin(1, 4) }, ENCODER_B_PIN{ pin(0, 7) }, ENCODER_Z_PIN{ pin(6, 7) };
        constexpr std::uint64_t DIRECTION_MARGIN_NS{ 5000 }, MINIMUM_PULSE_HIGH_NS{ 2500 };
        constexpr std::size_t PUBLICATION_CUT_ARGUMENT{ 5 };
        constexpr double MAX_SHAFT_SLIP_DEGREES{ 36000 };
        constexpr std::uint32_t MIN_PUBLICATION_LEAD{ 20'000 };
        std::int64_t encoder(double degrees)
        {
            return static_cast<std::int64_t>(
              std::floor((degrees * double(ENCODER_COUNTS_PER_REVOLUTION) / DEGREES_PER_REVOLUTION) +
                         ENCODER_ROUNDING_TOLERANCE));
        }
    }
    Machine::Machine(SharedMemory& memory,
                     Profile profile,
                     const std::filesystem::path& media,
                     std::optional<Configuration> configuration)
      : m_memory{ memory }
      , m_profile{ profile }
      , m_configuration{ configuration }
      , m_kinematics{ profile }
      , m_contacts{ profile }
      , m_media{ media }
      , m_peripherals{ m_memory,
                       [this](unsigned index, bool high, std::uint64_t time) { output(index, high, time); } }
    {
        m_profile.validate();
        output(ENABLE_PIN, true, m_time);
        m_step.trace_edges = false;
        m_step.interrupt = [this] {
            m_interrupt |= 2U;
            m_step.yield_requested = true;
        };
        m_step.edge_callback = [this](const auto& value) { edge(value); };
        sensors();
        publish();
    }
    Machine::Configuration Machine::configuration(const Profile& profile)
    {
        // Calibration of the simulated geometry, independent of the
        // controller's commanded/reference positions.
        Configuration values{};
        profile.validate();
        for (unsigned i{ 0 }; i < 3; ++i) {
            values[i] = profile.ratio[i];
            values[mechanical::AXIS_REFERENCE + i] = profile.axis_reference[i];
            values[mechanical::SHAFT_REFERENCE + i] = profile.shaft_reference[i];
            values[mechanical::DIRECTION + i] = double(profile.direction[i]);
            values[mechanical::GEOMETRY + i] = profile.geometry_mm[i];
            const auto upper{ ContactSensors::boundary(profile, i, true) };
            const auto lower{ i < 2 ? ContactSensors::boundary(profile, i, false)
                                    : ContactSensors::Boundary{} };
            const auto units{ i < 2 ? 1. : MILLIMETRES_PER_METRE };
            values[mechanical::UPPER_CONTACT + i] = upper.operate / units;
            values[mechanical::LOWER_CONTACT + i] = lower.operate / units;
            // Soft endpoints lie inside actuation and outside the final homing
            // release. Keep the switch datum separate from usable travel.
            values[mechanical::SOFT_MINIMUM + i] =
              (lower.operate + lower.release) / (MIDPOINT_DIVISOR * units);
            values[mechanical::SOFT_MAXIMUM + i] =
              (upper.operate + upper.release) / (MIDPOINT_DIVISOR * units);
        }
        values[mechanical::LEAD] = profile.z_mm_per_revolution;
        values[mechanical::TOOL_Z] = cad::REST_TCP_Z_MM;
        return values;
    }
    void Machine::record(std::uint64_t time, unsigned index, bool high) noexcept
    {
        if (!m_traceEnabled) {
            return;
        }
        if (m_traceCount == m_trace.size()) {
            m_traceOverflow = true;
            return;
        }
        m_trace[m_traceCount++] = { .time_ns = time, .pin = index, .high = high };
    }
    void Machine::input(unsigned index, bool value)
    {
        if (acquire(m_memory.gpio[index]) != std::uint32_t(value)) {
            record(m_time, index, value);
            release(m_memory.gpio[index], std::uint32_t(value));
            m_interrupt |= 4U;
            m_step.yield_requested = true;
        }
    }
    void Machine::sensors()
    {
        const auto joints{ m_kinematics.joints(m_shaft) };
        for (unsigned i{}; i < 3; ++i) {
            m_reference[i] = m_contacts.update(i, joints[i]);
            input(REF_PINS[i],
                  m_reference[i] || m_fault[i] == Fault::ReferenceStuck ||
                    m_fault[i] == Fault::ReferenceDisconnected);
            input(ALARM_PINS[i], m_fault[i] == Fault::DriverAlarm);
            if (i) {
                auto status{ m_tmc.getRegister(i - 1, tmc::DRV_STATUS) };
                status = m_fault[i] == Fault::DriverAlarm ? (status | (1U << 2)) : (status & ~(1U << 2));
                m_tmc.setRegister(i - 1, tmc::DRV_STATUS, status);
            }
        }
        const auto count{ encoder(m_shaft[0]) };
        const auto phase{ static_cast<unsigned>((count % 4 + 4) % 4) };
        if (m_fault[0] != Fault::EncoderDisconnected) {
            if (m_encoderRunning) {
                m_encoderCount += count - m_encoderLast;
            }
            // A/B are decoded locally by the emulated hardware counter; only Z is an EXTI source.
            release(m_memory.gpio[ENCODER_A_PIN], std::uint32_t(phase == 1 || phase == 2));
            release(m_memory.gpio[ENCODER_B_PIN], std::uint32_t(phase == 2 || phase == 3));
            input(ENCODER_Z_PIN,
                  ((count % ENCODER_COUNTS_PER_REVOLUTION + ENCODER_COUNTS_PER_REVOLUTION) %
                   ENCODER_COUNTS_PER_REVOLUTION) < 4);
        }
        m_encoderLast = count;
        release(m_memory.encoder_count, std::bit_cast<std::uint64_t>(m_encoderCount));
        for (unsigned i{ 1 }; i < 3; ++i) {
            const auto phase_steps{ static_cast<std::int64_t>(
              std::floor(m_electrical[i] / FULL_STEP_DEGREES * double(tmc::MAX_MICROSTEPS))) };
            const auto phase256{ static_cast<unsigned>(
              (phase_steps % ELECTRICAL_PHASE_STEPS + ELECTRICAL_PHASE_STEPS) % ELECTRICAL_PHASE_STEPS) };
            m_tmc.setRegister(i - 1, tmc::MSCNT, phase256);
            input(INDEX_PINS[i - 1], phase256 < INDEX_HIGH_STEPS);
        }
    }
    void Machine::output(unsigned index, bool high, std::uint64_t time)
    {
        if (acquire(m_memory.gpio[index]) == std::uint32_t(high)) {
            return;
        }
        for (unsigned i{}; i < 3; ++i) {
            if (index == DIR_PINS[i]) {
                if (acquire(m_memory.gpio[STEP_PINS[i]]) ||
                    (m_rise[i] && time - m_rise[i] < DIRECTION_MARGIN_NS)) {
                    ++m_rejectedPulses[i];
                    m_pendingStep[i] = false;
                }
                m_directionTime[i] = time;
            }
        }
        m_outputs[index] = high;
        record(time, index, high);
        release(m_memory.gpio[index], std::uint32_t(high));
        if (index == ENABLE_PIN && high) {
            m_pendingStep = {};
        }
        for (unsigned i{}; i < 3; ++i) {
            if (index == STEP_PINS[i]) {
                stepEdge(i, high, time);
            }
        }
    }
    void Machine::edge(const hal::util::SimulatedStepHardware::Edge& edge)
    {
        output(STEP_PINS[edge.axis], edge.high, m_edgeEpoch + (edge.tick * hal::util::STEP_TICK_NS));
    }
    void Machine::stepEdge(unsigned axis, bool high, std::uint64_t t)
    {
        if (!high) {
            if (!m_pendingStep[axis]) {
                return;
            }
            m_pendingStep[axis] = false;
            if (t < m_rise[axis] || t - m_rise[axis] < MINIMUM_PULSE_HIGH_NS) {
                ++m_rejectedPulses[axis];
                return;
            }
            if (!m_power || m_outputs[ENABLE_PIN] || m_fault[axis] == Fault::DriverAlarm) {
                return;
            }
            // An ideal accepted pulse advances mechanics after its minimum high time.
            // Timing violations do not invent an ALARM output on the physical driver.
            const int direction{ m_pendingDirection[axis] };
            const double delta{ direction * FULL_STEP_DEGREES / m_pendingResolution[axis] };
            m_steps[axis] += direction;
            m_electrical[axis] += delta;
            if (m_fault[axis] != Fault::LostStep) {
                m_shaft[axis] += delta;
            }
            const auto previous{ m_time };
            m_time = t;
            sensors();
            m_time = previous;
            return;
        }
        m_rise[axis] = t;
        m_pendingStep[axis] = false;
        if (!m_power || m_outputs[ENABLE_PIN] || m_fault[axis] == Fault::DriverAlarm) {
            return;
        }
        if (t < m_directionTime[axis] || t - m_directionTime[axis] < DIRECTION_MARGIN_NS) {
            ++m_rejectedPulses[axis];
            return;
        }
        unsigned resolution{ m_profile.dm542_microsteps };
        if (axis) {
            resolution = m_tmc.microsteps(axis - 1);
        }
        m_pendingDirection[axis] = m_outputs[DIR_PINS[axis]] ? 1 : -1;
        m_pendingResolution[axis] = resolution;
        m_pendingStep[axis] = true;
        sensors();
    }
    void Machine::advance(std::uint64_t nanoseconds)
    {
        if (m_lease || m_disconnected) {
            return;
        }
        if (nanoseconds > UINT64_MAX - m_time) {
            throw std::overflow_error{ "simulation time overflow" };
        }
        const auto end{ m_peripherals.deadline(m_time, m_time + nanoseconds) };
        if (m_step.registers.running) {
            const auto before{ m_step.elapsed };
            m_edgeEpoch = m_stepClock - (before * hal::util::STEP_TICK_NS);
            m_step.yield_requested = false;
            m_step.advance((end - m_stepClock) / hal::util::STEP_TICK_NS);
            m_stepClock += (m_step.elapsed - before) * hal::util::STEP_TICK_NS;
            m_time = m_step.yield_requested ? m_stepClock : end;
        }
        else {
            m_time = end;
        }
        publish();
    }
    void Machine::publish()
    {
        const auto sample{ m_step.sample() };
        release(m_memory.time_ns, m_time);
        release(m_memory.step_sample[0], sample.tick);
        release(m_memory.step_sample[1], std::uint32_t(sample.running));
        release(m_memory.step_sample[2], std::uint32_t(sample.error));
        for (unsigned i{}; i < 3; ++i) {
            const auto& c{ sample.channels[i] };
            const auto offset{ step_wire::HEADER_WORDS + (i * step_wire::CHANNEL_WORDS) };
            release(m_memory.step_sample[offset], c.tick);
            release(m_memory.step_sample[offset + 1], c.compare);
            release(m_memory.step_sample[offset + 2], c.remaining);
            release(m_memory.step_sample[offset + 3], std::uint32_t(c.target));
            release(m_memory.step_sample[offset + 4], std::uint32_t(c.transfer_complete));
            release(m_memory.step_sample[offset + step_wire::PENDING_FIELD],
                    std::uint32_t(c.compare_pending));
            release(m_memory.step_sample[offset + step_wire::HIGH_FIELD], std::uint32_t(c.high));
        }
        m_interrupt |= m_peripherals.advance(m_time);
        ++m_sequence;
    }
    PlantSnapshot Machine::snapshot() const
    {
        PlantSnapshot result{ .time_ns = m_time,
                              .sequence = m_sequence,
                              .session = m_memory.session,
                              .shaft_degrees = m_shaft,
                              .accepted_steps = m_steps };
        result.joints = m_kinematics.joints(m_shaft);
        result.tcp_mm = Kinematics::tcp(m_profile, result.joints);
        for (unsigned i{}; i < 3; ++i) {
            if (acquire(m_memory.gpio[REF_PINS[i]])) {
                result.references |= 1U << i;
            }
            if (m_fault[i] != Fault::None) {
                result.faults |= 1U << i;
            }
        }
        result.enabled = !m_outputs[ENABLE_PIN];
        result.powered = m_power;
        result.connected = !m_disconnected && acquire(m_memory.client_ready) != 0;
        return result;
    }
    void Machine::disconnect() noexcept
    {
        if (m_disconnected) {
            return;
        }
        m_disconnected = true;
        // Withdraw drive before forcing STEP low: cleanup must not invent a
        // final mechanical step from a half-finished pulse.
        m_pendingStep = {};
        if (!m_outputs[ENABLE_PIN]) {
            record(m_time, ENABLE_PIN, true);
        }
        m_outputs[ENABLE_PIN] = 1;
        release(m_memory.gpio[ENABLE_PIN], 1U);
        m_edgeEpoch = m_time - (m_step.elapsed * hal::util::STEP_TICK_NS);
        m_step.stop();
        for (const auto index : STEP_PINS) {
            output(index, false, m_time);
        }
        m_peripherals.stop(m_time);
        m_lease = false;
        publish();
        m_interrupt = 0;
    }
    bool Machine::stepCommand(const Packet& q, Packet& r)
    {
        const auto axis{ q.arguments[0] };
        switch (q.operation) {
            case Operation::Reset:
                m_pendingStep = {};
                m_edgeEpoch = m_time - (m_step.elapsed * hal::util::STEP_TICK_NS);
                m_step.stop();
                m_step.reset();
                break;
            case Operation::Start:
                if (axis > UINT32_MAX) {
                    r.status = EINVAL;
                    break;
                }
                m_stepClock = m_time;
                m_step.start(static_cast<std::uint32_t>(axis));
                break;
            case Operation::Guard:
                if (axis > UINT32_MAX) {
                    r.status = EINVAL;
                    break;
                }
                m_step.guard(static_cast<std::uint32_t>(axis));
                break;
            case Operation::Watch:
                if (axis > 1) {
                    r.status = EINVAL;
                    break;
                }
                m_step.completionWatch(axis != 0);
                break;
            case Operation::Sample:
                break;
            case Operation::Arm:
                if (axis >= 3 || q.arguments[1] > UINT32_MAX ||
                    q.arguments[2] > hal::util::STEP_BUFFER_EDGES || q.arguments[2] == 0 ||
                    !m_step.arm(axis,
                                static_cast<std::uint32_t>(q.arguments[1]),
                                static_cast<std::uint32_t>(q.arguments[2]))) {
                    r.status = EINVAL;
                }
                break;
            case Operation::Acknowledge:
                if (axis < 3) {
                    m_step.acknowledge(axis);
                }
                else {
                    r.status = EINVAL;
                }
                break;
            case Operation::FinishAxis:
                if (axis < 3) {
                    m_step.finishAxis(axis);
                }
                else {
                    r.status = EINVAL;
                }
                break;
            case Operation::StopAxis:
                if (axis < 3) {
                    m_step.stopAxis(axis);
                }
                else {
                    r.status = EINVAL;
                }
                break;
            case Operation::Stop:
                m_step.stop();
                break;
            default:
                return false;
        }
        return true;
    }
    bool Machine::gpioCommand(const Packet& q, Packet& r)
    {
        const auto axis{ q.arguments[0] };
        switch (q.operation) {
            case Operation::GpioConfigure:
                if (axis >= m_outputs.size()) {
                    r.status = EINVAL;
                    break;
                }
                m_isOutput[axis] = q.arguments[1] != 0;
                if (m_isOutput[axis]) {
                    output(static_cast<unsigned>(axis), q.arguments[2] != 0, m_time);
                }
                else {
                    sensors();
                }
                break;
            case Operation::GpioRead:
                if (axis < m_outputs.size()) {
                    r.arguments[0] = acquire(m_memory.gpio[axis]);
                }
                else {
                    r.status = EINVAL;
                }
                break;
            case Operation::GpioWrite:
                if (axis >= m_outputs.size()) {
                    r.status = EINVAL;
                    break;
                }
                output(static_cast<unsigned>(axis), q.arguments[1] != 0, m_time);
                break;
            case Operation::EncoderRun:
                if (axis > 1) {
                    r.status = EINVAL;
                    break;
                }
                m_encoderRunning = axis != 0;
                m_encoderLast = encoder(m_shaft[0]);
                break;
            case Operation::EncoderRead:
                r.arguments[0] = std::bit_cast<std::uint64_t>(m_encoderCount);
                break;
            case Operation::EncoderReset:
                m_encoderCount = std::bit_cast<std::int64_t>(axis);
                release(m_memory.encoder_count, axis);
                break;
            default:
                return false;
        }
        return true;
    }
    bool Machine::validFuturePublication(const Packet& q, std::span<const std::byte> bytes) const
    {
        const auto axis{ q.arguments[0] };
        const auto bank{ q.arguments[1] };
        const auto cut{ std::uint32_t(q.arguments[PUBLICATION_CUT_ARGUMENT]) };
        const auto future{ [cut](std::uint32_t tick) {
            const auto distance{ hal::util::step_distance(cut, tick) };
            return tick == hal::util::STEP_PARK || (distance && distance < hal::util::STEP_HORIZON);
        } };
        const auto lead{ hal::util::step_distance(m_step.registers.tick, cut) };
        if (q.arguments[PUBLICATION_CUT_ARGUMENT] > UINT32_MAX || lead < MIN_PUBLICATION_LEAD ||
            lead >= hal::util::STEP_HORIZON) {
            return false;
        }
        // A parked, LOW channel may rebase its pulse buffers
        // before a future Arm. Active DMA still protects every
        // already committed entry, including its prefetched word.
        for (unsigned i{ 0 }; m_step.active[axis] && i < m_step.entries[axis]; ++i) {
            const auto previous{ m_step.memory[axis][bank][i] };
            const auto replacement{ std::uint32_t(get(bytes, static_cast<std::size_t>(i) * 4, 4)) };
            if (previous != replacement && (!future(previous) || !future(replacement))) {
                return false;
            }
        }
        return true;
    }
    void Machine::retargetCommand(const Packet& q, Packet& r)
    {
        const auto axis{ q.arguments[0] };
        if (axis >= 3 || q.arguments[1] > UINT32_MAX || q.arguments[2] > UINT32_MAX) {
            r.status = EINVAL;
            return;
        }
        const auto cut{ std::uint32_t(q.arguments[2]) };
        const auto future{ [cut](std::uint32_t tick) {
            const auto distance{ hal::util::step_distance(cut, tick) };
            return tick == hal::util::STEP_PARK || (distance && distance < hal::util::STEP_HORIZON);
        } };
        const auto lead{ hal::util::step_distance(m_step.registers.tick, cut) };
        if (lead < MIN_PUBLICATION_LEAD || lead >= hal::util::STEP_HORIZON ||
            !future(m_step.registers.channels[axis].compare) || !future(std::uint32_t(q.arguments[1]))) {
            r.status = EINVAL;
        }
        else {
            m_step.retarget(axis, std::uint32_t(q.arguments[1]));
        }
    }
    bool Machine::publicationCommand(const Packet& q, Packet& r)
    {
        const auto axis{ q.arguments[0] };
        switch (q.operation) {
            case Operation::PublishFuture:
            case Operation::Publish: {
                const auto bank{ q.arguments[1] };
                const auto slot{ q.arguments[2] };
                if (axis >= 3 || bank >= 2 || slot >= BULK_COUNT ||
                    acquire(m_memory.bulk_generation[slot]) != q.arguments[3]) {
                    r.status = EINVAL;
                    break;
                }
                if (q.operation == Operation::Publish && m_step.active[axis] &&
                    m_step.registers.channels[axis].target == bank) {
                    r.status = EBUSY;
                    break;
                }
                const auto bytes{ std::span{ m_memory.bulk[slot] }.first(sizeof(hal::util::StepBuffer)) };
                if (checksum(bytes) != q.arguments[4]) {
                    r.status = EILSEQ;
                    break;
                }
                if (q.operation == Operation::PublishFuture) {
                    if (!validFuturePublication(q, bytes)) {
                        r.status = EINVAL;
                    }
                    if (r.status) {
                        break;
                    }
                }
                for (std::size_t i{}; i < hal::util::STEP_BUFFER_EDGES; ++i) {
                    m_step.memory[axis][bank][i] =
                      static_cast<std::uint32_t>(get(bytes, i * wire::U32_BYTES, wire::U32_BYTES));
                }
                break;
            }
            case Operation::Retarget:
                retargetCommand(q, r);
                break;
            default:
                return false;
        }
        return true;
    }
    bool Machine::driverCommand(const Packet& q, Packet& r)
    {
        const auto axis{ q.arguments[0] };
        switch (q.operation) {
            case Operation::Uart:
                uartCommand(q, r);
                break;
            case Operation::Fault:
                faultCommand(q, r);
                break;
            case Operation::Power:
                if (axis > 1) {
                    r.status = EINVAL;
                    break;
                }
                m_power = axis != 0;
                // This is the motor supply, not an MCU reset or an enable
                // command. Controller counters, GPIO and driver logic remain
                // live. A pulse spanning a supply loss cannot move the plant.
                if (!m_power) {
                    m_pendingStep = {};
                }
                break;
            default:
                return false;
        }
        return true;
    }
    bool Machine::systemCommand(const Packet& q, Packet& r)
    {
        switch (q.operation) {
            case Operation::Hello:
                r.arguments = { VERSION, hal::util::STEP_TICK_NS, 3, m_profile.dm542_microsteps };
                break;
            case Operation::Begin:
                m_lease = true;
                break;
            case Operation::End:
                m_lease = false;
                break;
            case Operation::Configuration: {
                if (!m_configuration) {
                    m_configuration = configuration(m_profile);
                }
                const auto& values{ *m_configuration };
                for (unsigned i{ 0 }; i < values.size(); ++i) {
                    put(r.data,
                        static_cast<std::size_t>(i) * state_wire::VALUE_BYTES,
                        std::bit_cast<std::uint64_t>(values[i]),
                        state_wire::VALUE_BYTES);
                }
                r.size = values.size() * state_wire::VALUE_BYTES;
                break;
            }
            case Operation::State: {
                const auto s{ snapshot() };
                r.arguments[0] = s.references;
                r.arguments[1] = s.faults;
                r.arguments[2] = s.enabled;
                r.arguments[3] = s.powered;
                for (unsigned i{}; i < 3; ++i) {
                    put(r.data,
                        static_cast<std::size_t>(i) * state_wire::VALUE_BYTES,
                        std::bit_cast<std::uint64_t>(s.shaft_degrees[i]),
                        state_wire::VALUE_BYTES);
                    put(r.data,
                        state_wire::JOINT + (std::size_t(i) * state_wire::VALUE_BYTES),
                        std::bit_cast<std::uint64_t>(s.joints[i]),
                        state_wire::VALUE_BYTES);
                    put(r.data,
                        state_wire::TCP + (std::size_t(i) * state_wire::VALUE_BYTES),
                        std::bit_cast<std::uint64_t>(s.tcp_mm[i]),
                        state_wire::VALUE_BYTES);
                }
                for (unsigned i{}; i < 3; ++i) {
                    put(r.data,
                        state_wire::REJECTED + (std::size_t(i) * state_wire::VALUE_BYTES),
                        m_rejectedPulses[i],
                        state_wire::VALUE_BYTES);
                }
                r.size = state_wire::BYTES;
                break;
            }
            case Operation::Shutdown:
                disconnect();
                release(m_memory.closed, 1U);
                break;
            case Operation::PwmConfigure:
            case Operation::PwmStart:
            case Operation::PwmStop:
            case Operation::TimerConfigure:
            case Operation::TimerRead:
            case Operation::TimerStart:
            case Operation::TimerStop:
                m_peripherals.execute(q, r, m_time);
                break;
            case Operation::MediaInfo:
            case Operation::MediaFlush:
            case Operation::MediaRead:
            case Operation::MediaWrite:
            case Operation::MediaErase:
                m_media.execute(q, r);
                break;
            default:
                return false;
        }
        return true;
    }
    void Machine::uartCommand(const Packet& q, Packet& r)
    {
        const auto axis{ q.arguments[0] };
        if (q.size > tmc::FRAME_BYTES || axis > tmc::FRAME_BYTES) {
            r.status = EINVAL;
            return;
        }
        std::array<std::uint8_t, tmc::FRAME_BYTES> tx{};
        std::array<std::uint8_t, tmc::FRAME_BYTES> rx{};
        for (unsigned i{}; i < q.size; ++i) {
            tx[i] = std::to_integer<std::uint8_t>(q.data[i]);
        }
        const auto result{ m_tmc.exchange(
          std::span{ tx }.first(q.size), std::span{ rx }.first(axis), tmc::TRANSACTION_TIMEOUT) };
        if (!result) {
            r.status = static_cast<std::uint32_t>(result.error().value());
        }
        else {
            r.size = static_cast<std::uint32_t>(axis);
            for (unsigned i{}; i < r.size; ++i) {
                r.data[i] = std::byte{ rx[i] };
            }
            if (q.size >= 2 && tx[1] < 2 && m_fault[tx[1] + 1] == Fault::UartCorrupt && r.size) {
                r.data[r.size - 1] ^= std::byte{ 1 };
            }
        }
    }
    void Machine::faultCommand(const Packet& q, Packet& r)
    {
        const auto axis{ q.arguments[0] };
        if (axis >= 3 || q.arguments[1] > std::to_underlying(Fault::UartCorrupt)) {
            r.status = EINVAL;
            return;
        }
        if ((q.arguments[1] == std::to_underlying(Fault::EncoderDisconnected) && axis != 0) ||
            ((q.arguments[1] == std::to_underlying(Fault::UartDisconnected) ||
              q.arguments[1] == std::to_underlying(Fault::UartCorrupt)) &&
             axis == 0)) {
            r.status = EINVAL;
            return;
        }
        if (q.arguments[1] == std::to_underlying(Fault::ShaftSlip)) {
            const auto slip{ std::bit_cast<double>(q.arguments[2]) };
            if (!std::isfinite(slip) || std::abs(slip) > MAX_SHAFT_SLIP_DEGREES) {
                r.status = EINVAL;
                return;
            }
            m_shaft[axis] += slip;
        }
        m_fault[axis] = static_cast<Fault>(q.arguments[1]);
        m_step.interrupts_enabled =
          std::ranges::none_of(m_fault, [](Fault f) { return f == Fault::InterruptDelay; });
        if (axis) {
            m_tmc.setConnected(static_cast<unsigned>(axis - 1), m_fault[axis] != Fault::UartDisconnected);
        }
        m_step.hold_dma[axis] = m_fault[axis] == Fault::DmaDelay;
        sensors();
    }
    Packet Machine::execute(const Packet& q)
    {
        Packet r{ q };
        r.status = 0;
        r.time_ns = m_time;
        r.size = 0;
        if (m_disconnected && q.operation != Operation::State) {
            r.status = ENOTCONN;
            return r;
        }
        if (q.status || q.size > PAYLOAD_BYTES) {
            r.status = EINVAL;
            return r;
        }
        if (q.session != m_memory.session) {
            r.status = ESTALE;
            return r;
        }
        if (q.sequence && q.sequence == m_lastRequest) {
            if (encode(q) == encode(m_lastCommand)) {
                return m_lastReply;
            }
            r.status = EILSEQ;
            return r;
        }
        if (q.sequence && q.sequence < m_lastRequest) {
            r.status = ESTALE;
            return r;
        }
        try {
            if (!(stepCommand(q, r) || gpioCommand(q, r) || publicationCommand(q, r) || driverCommand(q, r) ||
                  systemCommand(q, r))) {
                r.status = ENOTSUP;
            }
        } catch (const std::exception&) {
            r.status = EINVAL;
        }
        publish();
        r.time_ns = m_time;
        if (q.sequence) {
            m_lastRequest = q.sequence;
            m_lastCommand = q;
            m_lastReply = r;
        }
        return r;
    }
}
