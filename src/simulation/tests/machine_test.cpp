#include "../Machine.hpp"
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error{ message };
    }
}
int main()
try {
    using namespace simulation;
    Packet original{
        .operation = Operation::Arm, .size = 2, .sequence = 123, .session = 456, .arguments = { 1, 2, 3 }
    };
    original.data[0] = std::byte{ 42 };
    const auto decoded{ decode(encode(original)) };
    require(decoded.sequence == 123 && decoded.session == 456 && decoded.arguments == original.arguments &&
              decoded.data == original.data,
            "codec roundtrip");
    auto malformed{ encode(original) };
    malformed[0] = std::byte{ 255 };
    bool rejected{};
    try {
        decode(malformed);
    } catch (...) {
        rejected = true;
    }
    require(rejected, "codec version validation");
    Mapping mapping;
    auto& shared{ mapping.memory() };
    auto& ring{ shared.incoming[0][0] };
    release(ring.read, std::numeric_limits<std::uint32_t>::max() - 3);
    release(ring.written, acquire(ring.read));
    const auto bytes{ encode(original) };
    for (unsigned i{}; i < 8; ++i)
        require(enqueue(ring, bytes) == QueueResult::Success, "ring enqueue");
    require(enqueue(ring, bytes) == QueueResult::Full, "ring backpressure");
    std::array<std::byte, FRAME_BYTES> output{};
    std::size_t size{};
    require(dequeue(ring, std::span{ output }.first(1), size) == QueueResult::BufferTooSmall,
            "ring preserves oversized frame");
    for (unsigned i{}; i < 8; ++i)
        require(dequeue(ring, output, size) == QueueResult::Success && size == PACKET_BYTES,
                "ring wrap decode");
    require(dequeue(ring, output, size) == QueueResult::Empty, "ring empty");
    const auto child{ fork() };
    if (child == 0) {
        Mapping peer{ mapping.fd() };
        _exit(enqueue(peer.memory().incoming[0][0], bytes) == QueueResult::Success ? 0 : 1);
    }
    require(child > 0, "fork");
    int status{};
    waitpid(child, &status, 0);
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child mapping");
    require(dequeue(ring, output, size) == QueueResult::Success, "interprocess publication");
    Machine machine{ shared };
    std::uint64_t sequence{};
    const auto command{ [&](Operation op, std::array<std::uint64_t, 8> args = {}) {
        return machine.execute(
          Packet{ .operation = op, .sequence = ++sequence, .session = shared.session, .arguments = args });
    } };
    require(command(Operation::Hello).status == 0, "handshake");
    command(Operation::Begin);
    machine.advance(1000000);
    require(machine.time() == 0, "lease holds logical time");
    command(Operation::End);
    command(Operation::GpioWrite, { ENABLE_PIN, 0 });
    command(Operation::GpioWrite, { DIR_PINS[0], 1 });
    machine.advance(10000);
    command(Operation::Reset);
    command(Operation::Start, { hal::util::STEP_PARK });
    const std::array<std::uint32_t, 4> edges{ 150, 250, 350, hal::util::STEP_PARK };
    for (unsigned i{}; i < edges.size(); ++i)
        put(shared.bulk[0], i * 4, edges[i], 4);
    release(shared.bulk_generation[0], 1U);
    require(
      command(Operation::Publish,
              { 0, 0, 0, 1, checksum(std::span{ shared.bulk[0] }.first(sizeof(hal::util::StepBuffer))) })
          .status == 0,
      "bank publication");
    require(command(Operation::Arm, { 0, 100, 4 }).status == 0, "arm");
    for (unsigned i{}; i < 10; ++i) {
        machine.advance(10000);
        machine.takeInterrupt();
    }
    require(machine.snapshot().accepted_steps[0] == 2, "exact pulse count");
    const auto pose{ machine.snapshot() };
    command(Operation::EncoderReset, { 100 });
    require(machine.snapshot().shaft_degrees == pose.shaft_degrees, "reference does not move mechanism");
    require(command(Operation::EncoderRead).arguments[0] == 100, "encoder reference");
    command(Operation::Fault,
            { 0, std::to_underlying(Fault::ShaftSlip), std::bit_cast<std::uint64_t>(10.0) });
    require(machine.snapshot().accepted_steps == pose.accepted_steps, "slip is not commanded steps");
    require(machine.snapshot().shaft_degrees[0] > pose.shaft_degrees[0] + 9.9,
            "slip changes mechanical truth");
    require(command(Operation::Arm, { 4, 1, 4 }).status != 0, "invalid axis rejection");

    // Idempotence is exact: a conflicting duplicate cannot apply a second mutation.
    Packet duplicate{ .operation = Operation::GpioWrite,
                      .sequence = ++sequence,
                      .session = shared.session,
                      .arguments = { ENABLE_PIN, 1 } };
    auto first{ machine.execute(duplicate) };
    require(first.status == 0, "duplicate original");
    require(machine.execute(duplicate).status == 0, "duplicate replay acknowledgement");
    duplicate.arguments[1] = 0;
    require(machine.execute(duplicate).status == EILSEQ && !machine.snapshot().enabled,
            "conflicting replay rejected atomically");
    duplicate.session ^= 1;
    duplicate.sequence = ++sequence;
    require(machine.execute(duplicate).status == ESTALE, "wrong session rejected");
    command(Operation::Stop);
    // The counter is a peripheral. Resetting its coordinate or disabling it never moves the shaft.
    command(Operation::Fault, { 0, std::to_underlying(Fault::None) });
    command(Operation::EncoderRun, { 1 });
    command(Operation::EncoderReset, { 0 });
    command(Operation::Fault,
            { 0, std::to_underlying(Fault::ShaftSlip), std::bit_cast<std::uint64_t>(360.0) });
    require(std::bit_cast<std::int64_t>(command(Operation::EncoderRead).arguments[0]) == 1600,
            "encoder quadrature resolution");
    command(Operation::EncoderRun, { 0 });
    command(Operation::Fault,
            { 0, std::to_underlying(Fault::ShaftSlip), std::bit_cast<std::uint64_t>(-720.0) });
    require(command(Operation::EncoderRead).arguments[0] == 1600, "stopped encoder does not count");
    // Geometry-derived contact at 60 degrees on J2; 45 degrees is clear.
    command(Operation::Fault,
            { 1, std::to_underlying(Fault::ShaftSlip), std::bit_cast<std::uint64_t>(300.0) });
    require(acquire(shared.gpio[REF_PINS[1]]) == 1, "collision activates reference");
    command(Operation::Fault,
            { 1, std::to_underlying(Fault::ShaftSlip), std::bit_cast<std::uint64_t>(-75.0) });
    require(acquire(shared.gpio[REF_PINS[1]]) == 0, "moving away from the lever releases reference");
    command(Operation::Fault, { 1, std::to_underlying(Fault::ReferenceDisconnected) });
    require(acquire(shared.gpio[REF_PINS[1]]) == 1, "NC disconnected contact is active");
    // Runtime timers advance only with device time, including wrap and pending interrupts.
    command(Operation::TimerConfigure, { 5, 0, 240000000, 239, 999 });
    command(Operation::TimerStart, { 5, 1 });
    const auto timer_start{ machine.time() };
    machine.takeInterrupt();
    machine.advance(999000);
    require(acquire(shared.timers[5][0]) == 999, "timer counter before wrap");
    machine.advance(1000);
    require(machine.time() == timer_start + 1000000, "timer deadline");
    require(acquire(shared.timers[5][0]) == 0 && acquire(shared.timers[5][6]) == 1 &&
              (machine.takeInterrupt() & 8),
            "timer update interrupt");
    command(Operation::TimerStop, { 5 });
    machine.advance(1000000);
    require(acquire(shared.timers[5][0]) == 0, "stopped timer frozen");
    // Raw devices preserve flash NOR programming rules without implementing a filesystem here.
    Packet write{ .operation = Operation::MediaWrite,
                  .size = 3,
                  .sequence = ++sequence,
                  .session = shared.session,
                  .arguments = { 0, 4096, 3 } };
    write.data[0] = std::byte{ 0x12 };
    write.data[1] = std::byte{ 0x34 };
    write.data[2] = std::byte{ 0x56 };
    require(machine.execute(write).status == 0, "NOR program");
    auto read{ command(Operation::MediaRead, { 0, 4096, 3 }) };
    require(read.size == 3 && read.data[1] == std::byte{ 0x34 }, "NOR readback");
    write.sequence = ++sequence;
    write.data[0] = std::byte{ 255 };
    require(machine.execute(write).status == EIO, "NOR rejects zero-to-one programming");
    require(command(Operation::MediaErase, { 0, 4096, 3 }).status == 0, "NOR erase");
    require(command(Operation::MediaRead, { 0, 4096, 3 }).data[0] == std::byte{ 255 }, "NOR erased state");
    require(command(Operation::MediaRead, { 1, 64U * 1024U * 1024U - 1, 2 }).status == EINVAL,
            "media range check");
    require(command(Operation::Fault,
                    { 1,
                      std::to_underlying(Fault::ShaftSlip),
                      std::bit_cast<std::uint64_t>(std::numeric_limits<double>::quiet_NaN()) })
                .status == EINVAL,
            "nonfinite physical injection rejected");
    // Native PWM keeps electrical edge count separate from ordinary uncounted output.
    require(command(Operation::PwmConfigure, { 0, pin(0, 8), 10000, 3000 }).status == 0, "PWM configure");
    require(command(Operation::PwmStart, { 0, 2, 1 }).status == 0, "PWM counted start");
    const auto pwm_start{ machine.time() };
    while (machine.time() < pwm_start + 23000) {
        machine.advance(pwm_start + 23000 - machine.time());
        machine.takeInterrupt();
    }
    require(acquire(shared.pwm[0][0]) == 2 && !acquire(shared.pwm[0][1]) && !acquire(shared.gpio[pin(0, 8)]),
            "PWM two complete pulses and idle low");
    require(command(Operation::PwmStart, { 0, 0, 0 }).status == 0 &&
              command(Operation::PwmStart, { 0, 0, 0 }).status == 0,
            "continuous PWM idempotent start");
    for (unsigned i{}; i < 8; ++i)
        machine.advance(10000);
    require(acquire(shared.pwm[0][0]) == 0, "continuous PWM does not count");
    command(Operation::PwmStop, { 0 });
    // Direct hardware register wrap must not alter absolute capture times.
    hal::util::SimulatedStepHardware hardware;
    hardware.registers.tick = hal::util::STEP_PARK - 100;
    hardware.memory[0][0][0] = hal::util::step_add(hardware.registers.tick, 150);
    hardware.memory[0][0][1] = hal::util::STEP_PARK;
    hardware.start(hal::util::STEP_PARK);
    require(hardware.arm(0, hal::util::step_add(hardware.registers.tick, 50), 2), "wrap arm");
    hardware.advance(200);
    require(hardware.edges.size() == 2 && hardware.edges[0].tick == 50 && hardware.edges[1].tick == 150 &&
              hardware.edges[0].high && !hardware.edges[1].high,
            "STEP wrap edge times");
    // TMC CRC, write acknowledgement, read-only fields and power reset.
    hal::device::Tmc2209Model tmc;
    const auto crc{ [](std::span<const std::uint8_t> bytes) {
        std::uint8_t value{};
        for (auto byte : bytes)
            for (unsigned bit{}; bit < 8; ++bit) {
                const bool feedback{ bool((value >> 7) ^ (byte & 1)) };
                value = static_cast<std::uint8_t>(value << 1);
                if (feedback)
                    value ^= 7;
                byte >>= 1;
            }
        return value;
    } };
    std::array<std::uint8_t, 8> write_register{ 0x05, 0, 0x80, 0, 0, 0, 0x41, 0 };
    write_register.back() = crc(std::span{ write_register }.first(7));
    require(bool(tmc.exchange(write_register, {}, std::chrono::milliseconds{ 10 })) &&
              tmc.getRegister(0, 0) == 0x41 && tmc.getRegister(0, 2) == 1,
            "TMC register write/IFCNT");
    write_register.back() ^= 1;
    require(!tmc.exchange(write_register, {}, std::chrono::milliseconds{ 10 }) && tmc.getRegister(0, 2) == 1,
            "TMC bad CRC no mutation");
    std::array<std::uint8_t, 4> read_register{ 0x05, 0, 0x02, 0 };
    read_register.back() = crc(std::span{ read_register }.first(3));
    std::array<std::uint8_t, 8> reply{};
    require(bool(tmc.exchange(read_register, reply, std::chrono::milliseconds{ 10 })) && reply[1] == 0xFF &&
              reply[6] == 1 && reply.back() == crc(std::span{ reply }.first(7)),
            "TMC response address/value/CRC");
    tmc.setConnected(0, false);
    require(!tmc.exchange(read_register, reply, std::chrono::milliseconds{ 10 }), "TMC disconnected timeout");
    // A tampered slot fails closed without being consumed.
    require(enqueue(ring, bytes) == QueueResult::Success, "corrupt slot fixture");
    ring.slots[acquire(ring.read) % QUEUE_CAPACITY].bytes[20] ^= std::byte{ 1 };
    require(dequeue(ring, output, size) == QueueResult::Corrupt, "shared frame checksum");
    machine.disconnect();
    require(!machine.snapshot().enabled, "disconnect disables plant");
    std::puts("PASS: serialization, bounded shared queues, process exchange, time lease, DMA pulses, "
              "mechanical truth, feedback");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
