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
        std::int64_t encoder(double degrees)
        {
            return static_cast<std::int64_t>(std::floor(degrees * 1600.0 / 360.0 + 1e-9));
        }
    }
    Machine::Machine(SharedMemory& memory, Profile profile, const std::filesystem::path& media)
      : m_memory{ memory }
      , m_profile{ profile }
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
    void Machine::record(std::uint64_t time, unsigned index, bool high) noexcept
    {
        if (!m_trace_enabled)
            return;
        if (m_trace_count == m_trace.size()) {
            m_trace_overflow = true;
            return;
        }
        m_trace[m_trace_count++] = { time, index, high };
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
                auto status{ m_tmc.getRegister(i - 1, 0x6F) };
                status = m_fault[i] == Fault::DriverAlarm ? (status | (1U << 2)) : (status & ~(1U << 2));
                m_tmc.setRegister(i - 1, 0x6F, status);
            }
        }
        const auto count{ encoder(m_shaft[0]) };
        const auto phase{ static_cast<unsigned>((count % 4 + 4) % 4) };
        if (m_fault[0] != Fault::EncoderDisconnected) {
            if (m_encoder_running)
                m_encoder_count += count - m_encoder_last;
            // A/B are decoded locally by the emulated hardware counter; only Z is an EXTI source.
            release(m_memory.gpio[pin(1, 4)], std::uint32_t(phase == 1 || phase == 2));
            release(m_memory.gpio[pin(0, 7)], std::uint32_t(phase == 2 || phase == 3));
            input(pin(6, 7), ((count % 1600 + 1600) % 1600) < 4);
        }
        m_encoder_last = count;
        release(m_memory.encoder_count, std::bit_cast<std::uint64_t>(m_encoder_count));
        for (unsigned i{ 1 }; i < 3; ++i) {
            const auto phase_steps{ static_cast<std::int64_t>(std::floor(m_electrical[i] / 1.8 * 256.0)) };
            const auto phase256{ static_cast<unsigned>((phase_steps % 1024 + 1024) % 1024) };
            m_tmc.setRegister(i - 1, 0x6A, phase256);
            input(INDEX_PINS[i - 1], phase256 < 256);
        }
    }
    void Machine::output(unsigned index, bool high, std::uint64_t time)
    {
        if (acquire(m_memory.gpio[index]) == std::uint32_t(high))
            return;
        for (unsigned i{}; i < 3; ++i)
            if (index == DIR_PINS[i]) {
                if (acquire(m_memory.gpio[STEP_PINS[i]]) || (m_rise[i] && time - m_rise[i] < 5000)) {
                    ++m_rejected_pulses[i];
                    m_pending_step[i] = false;
                }
                m_direction_time[i] = time;
            }
        m_outputs[index] = high;
        record(time, index, high);
        release(m_memory.gpio[index], std::uint32_t(high));
        if (index == ENABLE_PIN && high)
            m_pending_step = {};
        for (unsigned i{}; i < 3; ++i)
            if (index == STEP_PINS[i])
                stepEdge(i, high, time);
    }
    void Machine::edge(const hal::util::SimulatedStepHardware::Edge& edge)
    {
        output(STEP_PINS[edge.axis], edge.high, m_edge_epoch + edge.tick * hal::util::STEP_TICK_NS);
    }
    void Machine::stepEdge(unsigned axis, bool high, std::uint64_t t)
    {
        if (!high) {
            if (!m_pending_step[axis])
                return;
            m_pending_step[axis] = false;
            if (t < m_rise[axis] || t - m_rise[axis] < 2500) {
                ++m_rejected_pulses[axis];
                return;
            }
            if (!m_power || m_outputs[ENABLE_PIN] || m_fault[axis] == Fault::DriverAlarm)
                return;
            // An ideal accepted pulse advances mechanics after its minimum high time.
            // Timing violations do not invent an ALARM output on the physical driver.
            const int direction{ m_pending_direction[axis] };
            const double delta{ direction * 1.8 / m_pending_resolution[axis] };
            m_steps[axis] += direction;
            m_electrical[axis] += delta;
            if (m_fault[axis] != Fault::LostStep)
                m_shaft[axis] += delta;
            const auto previous{ m_time };
            m_time = t;
            sensors();
            m_time = previous;
            return;
        }
        m_rise[axis] = t;
        m_pending_step[axis] = false;
        if (!m_power || m_outputs[ENABLE_PIN] || m_fault[axis] == Fault::DriverAlarm)
            return;
        if (t < m_direction_time[axis] || t - m_direction_time[axis] < 5000) {
            ++m_rejected_pulses[axis];
            return;
        }
        unsigned resolution{ m_profile.dm542_microsteps };
        if (axis) {
            const auto chop{ m_tmc.getRegister(static_cast<unsigned>(axis - 1), 0x6C) };
            resolution = 256U >> std::min(8U, (chop >> 24U) & 15U);
        }
        m_pending_direction[axis] = m_outputs[DIR_PINS[axis]] ? 1 : -1;
        m_pending_resolution[axis] = resolution;
        m_pending_step[axis] = true;
        sensors();
    }
    void Machine::advance(std::uint64_t nanoseconds)
    {
        if (m_lease || m_disconnected)
            return;
        if (nanoseconds > UINT64_MAX - m_time)
            throw std::overflow_error{ "simulation time overflow" };
        const auto end{ m_peripherals.deadline(m_time, m_time + nanoseconds) };
        if (m_step.registers.running) {
            const auto before{ m_step.elapsed };
            m_edge_epoch = m_step_clock - before * hal::util::STEP_TICK_NS;
            m_step.yield_requested = false;
            m_step.advance((end - m_step_clock) / hal::util::STEP_TICK_NS);
            m_step_clock += (m_step.elapsed - before) * hal::util::STEP_TICK_NS;
            m_time = m_step.yield_requested ? m_step_clock : end;
        }
        else
            m_time = end;
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
            const auto offset{ 3 + i * 8 };
            release(m_memory.step_sample[offset], c.tick);
            release(m_memory.step_sample[offset + 1], c.compare);
            release(m_memory.step_sample[offset + 2], c.remaining);
            release(m_memory.step_sample[offset + 3], std::uint32_t(c.target));
            release(m_memory.step_sample[offset + 4], std::uint32_t(c.transfer_complete));
            release(m_memory.step_sample[offset + 5], std::uint32_t(c.compare_pending));
            release(m_memory.step_sample[offset + 6], std::uint32_t(c.high));
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
            if (acquire(m_memory.gpio[REF_PINS[i]]))
                result.references |= 1U << i;
            if (m_fault[i] != Fault::None)
                result.faults |= 1U << i;
        }
        result.enabled = !m_outputs[ENABLE_PIN];
        result.powered = m_power;
        result.connected = !m_disconnected && acquire(m_memory.client_ready) != 0;
        return result;
    }
    void Machine::disconnect() noexcept
    {
        if (m_disconnected)
            return;
        m_disconnected = true;
        // Withdraw drive before forcing STEP low: cleanup must not invent a
        // final mechanical step from a half-finished pulse.
        m_pending_step = {};
        if (!m_outputs[ENABLE_PIN])
            record(m_time, ENABLE_PIN, true);
        m_outputs[ENABLE_PIN] = 1;
        release(m_memory.gpio[ENABLE_PIN], 1U);
        m_edge_epoch = m_time - m_step.elapsed * hal::util::STEP_TICK_NS;
        m_step.stop();
        for (const auto index : STEP_PINS)
            output(index, false, m_time);
        m_peripherals.stop(m_time);
        m_lease = false;
        publish();
        m_interrupt = 0;
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
        if (q.sequence && q.sequence == m_last_request) {
            if (encode(q) == encode(m_last_command))
                return m_last_reply;
            r.status = EILSEQ;
            return r;
        }
        if (q.sequence && q.sequence < m_last_request) {
            r.status = ESTALE;
            return r;
        }
        const auto axis{ q.arguments[0] };
        const auto op{ q.operation };
        try {
            switch (op) {
                case Operation::Hello:
                    r.arguments = { VERSION, hal::util::STEP_TICK_NS, 3, m_profile.dm542_microsteps };
                    break;
                case Operation::Begin:
                    m_lease = true;
                    break;
                case Operation::End:
                    m_lease = false;
                    break;
                case Operation::Reset:
                    m_pending_step = {};
                    m_edge_epoch = m_time - m_step.elapsed * hal::util::STEP_TICK_NS;
                    m_step.stop();
                    m_step.reset();
                    break;
                case Operation::Start:
                    if (axis > UINT32_MAX) {
                        r.status = EINVAL;
                        break;
                    }
                    m_step_clock = m_time;
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
                                    static_cast<std::uint32_t>(q.arguments[2])))
                        r.status = EINVAL;
                    break;
                case Operation::Publish: {
                    const auto bank{ q.arguments[1] }, slot{ q.arguments[2] };
                    if (axis >= 3 || bank >= 2 || slot >= BULK_COUNT ||
                        acquire(m_memory.bulk_generation[slot]) != q.arguments[3]) {
                        r.status = EINVAL;
                        break;
                    }
                    if (m_step.active[axis] && m_step.registers.channels[axis].target == bank) {
                        r.status = EBUSY;
                        break;
                    }
                    const auto bytes{ std::span{ m_memory.bulk[slot] }.first(sizeof(hal::util::StepBuffer)) };
                    if (checksum(bytes) != q.arguments[4]) {
                        r.status = EILSEQ;
                        break;
                    }
                    for (std::size_t i{}; i < hal::util::STEP_BUFFER_EDGES; ++i)
                        m_step.memory[axis][bank][i] = static_cast<std::uint32_t>(get(bytes, i * 4, 4));
                    break;
                }
                case Operation::Acknowledge:
                    if (axis < 3)
                        m_step.acknowledge(axis);
                    else
                        r.status = EINVAL;
                    break;
                case Operation::FinishAxis:
                    if (axis < 3)
                        m_step.finishAxis(axis);
                    else
                        r.status = EINVAL;
                    break;
                case Operation::StopAxis:
                    if (axis < 3)
                        m_step.stopAxis(axis);
                    else
                        r.status = EINVAL;
                    break;
                case Operation::Stop:
                    m_step.stop();
                    break;
                case Operation::GpioConfigure:
                    if (axis >= m_outputs.size()) {
                        r.status = EINVAL;
                        break;
                    }
                    m_is_output[axis] = q.arguments[1] != 0;
                    if (m_is_output[axis]) {
                        output(static_cast<unsigned>(axis), q.arguments[2] != 0, m_time);
                    }
                    else
                        sensors();
                    break;
                case Operation::GpioRead:
                    if (axis < m_outputs.size())
                        r.arguments[0] = acquire(m_memory.gpio[axis]);
                    else
                        r.status = EINVAL;
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
                    m_encoder_running = axis != 0;
                    m_encoder_last = encoder(m_shaft[0]);
                    break;
                case Operation::EncoderRead:
                    r.arguments[0] = std::bit_cast<std::uint64_t>(m_encoder_count);
                    break;
                case Operation::EncoderReset:
                    m_encoder_count = std::bit_cast<std::int64_t>(axis);
                    release(m_memory.encoder_count, axis);
                    break;
                case Operation::Uart: {
                    if (q.size > 8 || axis > 8) {
                        r.status = EINVAL;
                        break;
                    }
                    std::array<std::uint8_t, 8> tx{}, rx{};
                    for (unsigned i{}; i < q.size; ++i)
                        tx[i] = std::to_integer<std::uint8_t>(q.data[i]);
                    const auto result{ m_tmc.exchange(std::span{ tx }.first(q.size),
                                                      std::span{ rx }.first(axis),
                                                      std::chrono::milliseconds{ 10 }) };
                    if (!result)
                        r.status = static_cast<std::uint32_t>(result.error().value());
                    else {
                        r.size = static_cast<std::uint32_t>(axis);
                        for (unsigned i{}; i < r.size; ++i)
                            r.data[i] = std::byte{ rx[i] };
                        if (q.size >= 2 && tx[1] < 2 && m_fault[tx[1] + 1] == Fault::UartCorrupt && r.size)
                            r.data[r.size - 1] ^= std::byte{ 1 };
                    }
                    break;
                }
                case Operation::Fault:
                    if (axis >= 3 || q.arguments[1] > std::to_underlying(Fault::UartCorrupt)) {
                        r.status = EINVAL;
                        break;
                    }
                    if ((q.arguments[1] == std::to_underlying(Fault::EncoderDisconnected) && axis != 0) ||
                        ((q.arguments[1] == std::to_underlying(Fault::UartDisconnected) ||
                          q.arguments[1] == std::to_underlying(Fault::UartCorrupt)) &&
                         axis == 0)) {
                        r.status = EINVAL;
                        break;
                    }
                    if (q.arguments[1] == std::to_underlying(Fault::ShaftSlip)) {
                        const auto slip{ std::bit_cast<double>(q.arguments[2]) };
                        if (!std::isfinite(slip) || std::abs(slip) > 36000) {
                            r.status = EINVAL;
                            break;
                        }
                        m_shaft[axis] += slip;
                    }
                    m_fault[axis] = static_cast<Fault>(q.arguments[1]);
                    m_step.interrupts_enabled = std::none_of(
                      m_fault.begin(), m_fault.end(), [](Fault f) { return f == Fault::InterruptDelay; });
                    if (axis)
                        m_tmc.setConnected(static_cast<unsigned>(axis - 1),
                                           m_fault[axis] != Fault::UartDisconnected);
                    m_step.hold_dma[axis] = m_fault[axis] == Fault::DmaDelay;
                    sensors();
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
                    if (!m_power)
                        m_pending_step = {};
                    break;
                case Operation::State: {
                    const auto s{ snapshot() };
                    r.arguments[0] = s.references;
                    r.arguments[1] = s.faults;
                    r.arguments[2] = s.enabled;
                    r.arguments[3] = s.powered;
                    for (unsigned i{}; i < 3; ++i) {
                        put(r.data, i * 8, std::bit_cast<std::uint64_t>(s.shaft_degrees[i]), 8);
                        put(r.data, 24 + i * 8, std::bit_cast<std::uint64_t>(s.joints[i]), 8);
                        put(r.data, 48 + i * 8, std::bit_cast<std::uint64_t>(s.tcp_mm[i]), 8);
                    }
                    for (unsigned i{}; i < 3; ++i)
                        put(r.data, 72 + i * 8, m_rejected_pulses[i], 8);
                    r.size = 96;
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
                    r.status = ENOTSUP;
                    break;
            }
        } catch (const std::exception&) {
            r.status = EINVAL;
        }
        publish();
        r.time_ns = m_time;
        if (q.sequence) {
            m_last_request = q.sequence;
            m_last_command = q;
            m_last_reply = r;
        }
        return r;
    }
}
