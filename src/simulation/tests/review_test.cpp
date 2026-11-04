#include "../Machine.hpp"
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <sys/mman.h>
#include <unistd.h>

namespace
{
    using namespace simulation;
    void require(bool value, const char* message)
    {
        if (!value)
            throw std::runtime_error{ message };
    }
    struct Fixture
    {
        Mapping mapping;
        Machine machine;
        std::uint64_t sequence{};
        explicit Fixture(Profile profile = {})
          : machine{ mapping.memory(), profile }
        {
        }
        Packet command(Operation op, std::array<std::uint64_t, 8> args = {})
        {
            return machine.execute({ .operation = op,
                                     .sequence = ++sequence,
                                     .session = mapping.memory().session,
                                     .arguments = args });
        }
        void advance(std::uint64_t ns)
        {
            const auto end{ machine.time() + ns };
            while (machine.time() < end) {
                machine.takeInterrupt();
                const auto before{ machine.time() };
                machine.advance(end - before);
                require(machine.time() > before, "device time failed to progress");
            }
        }
        void arm(unsigned axis = 0)
        {
            command(Operation::GpioWrite, { ENABLE_PIN, 0 });
            command(Operation::GpioWrite, { DIR_PINS[axis], 1 });
            command(Operation::Reset);
            command(Operation::Start, { hal::util::STEP_PARK });
            auto& shared{ mapping.memory() };
            shared.bulk[0].fill(std::byte{});
            put(shared.bulk[0], 0, 150, 4);
            put(shared.bulk[0], 4, hal::util::STEP_PARK, 4);
            release(shared.bulk_generation[0], 1U);
            require(
              command(
                Operation::Publish,
                { axis, 0, 0, 1, checksum(std::span{ shared.bulk[0] }.first(sizeof(hal::util::StepBuffer))) })
                  .status == 0,
              "pulse buffer publication");
            require(command(Operation::Arm, { axis, 100, 2 }).status == 0, "pulse arm");
        }
    };
    void timers()
    {
        Fixture f{};
        auto& timer{ f.mapping.memory().timers[5] };
        require(!f.command(Operation::TimerConfigure, { 5, 0, 240000000, 239, 999 }).status, "configure");
        require(!f.command(Operation::TimerStart, { 5, 1 }).status, "timer start");
        const auto original{ timer };
        for (const auto& args : { std::array<std::uint64_t, 8>{ 5, 0, 0, 239, 999 },
                                  std::array<std::uint64_t, 8>{ 5, 0, UINT64_MAX, 239, 999 },
                                  std::array<std::uint64_t, 8>{ 5, 0, 240000000, UINT64_MAX, 999 },
                                  std::array<std::uint64_t, 8>{ 5, 99, 10 } }) {
            require(f.command(Operation::TimerConfigure, args).status == EINVAL, "invalid timer accepted");
            require(timer == original, "invalid timer mutated registers");
        }
        f.advance(1000000);
        require(timer[6] == 1, "timer stopped after rejected configure");
        // 3 Hz / 2 is 1.5 Hz, not the 1 Hz produced by early integer division.
        f.command(Operation::TimerStop, { 5 });
        f.command(Operation::TimerConfigure, { 5, 0, 3, 1, 99 });
        f.command(Operation::TimerConfigure, { 5, 1, 0 });
        f.command(Operation::TimerStart, { 5, 0 });
        f.advance(2000000000);
        require(timer[0] == 3, "fractional divider frequency was truncated");
        f.command(Operation::TimerConfigure, { 5, 3, 1 });
        f.command(Operation::TimerStart, { 5, 1 });
        f.advance(1000000000);
        require(timer[0] <= 1, "counter/reload normalization");
    }
    void atomic_fault()
    {
        Fixture f{};
        require(!f.command(Operation::Fault, { 1, std::to_underlying(Fault::DriverAlarm) }).status, "alarm");
        const auto pose{ f.machine.snapshot() };
        require(f.command(Operation::Fault,
                          { 1,
                            std::to_underlying(Fault::ShaftSlip),
                            std::bit_cast<std::uint64_t>(std::numeric_limits<double>::quiet_NaN()) })
                    .status == EINVAL,
                "nonfinite slip accepted");
        require(f.machine.snapshot().faults == pose.faults &&
                  acquire(f.mapping.memory().gpio[ALARM_PINS[1]]) == 1,
                "rejected slip cleared existing alarm");
        require(f.machine.snapshot().shaft_degrees == pose.shaft_degrees, "rejected slip moved mechanism");
    }
    void power_and_reset()
    {
        Fixture f{};
        f.arm();
        f.advance(10000);
        require(acquire(f.mapping.memory().gpio[STEP_PINS[0]]) == 1, "rising edge fixture");
        f.command(Operation::Power, { 0 });
        require(acquire(f.mapping.memory().step_sample[1]) == 1 && f.machine.snapshot().enabled,
                "power fault changed controller timer or enable GPIO");
        f.command(Operation::Power, { 1 });
        f.advance(10000);
        require(f.machine.snapshot().accepted_steps[0] == 0, "power restoration accepted a partial pulse");
        f.arm();
        f.advance(20000);
        require(f.machine.snapshot().accepted_steps[0] == 1, "full pulse after power restoration");
        f.arm();
        f.advance(10000);
        f.command(Operation::Reset);
        require(acquire(f.mapping.memory().gpio[STEP_PINS[0]]) == 0, "reset left STEP high");
        require(f.machine.snapshot().accepted_steps[0] == 1, "reset invented a mechanical step");
        f.arm();
        f.command(Operation::Power, { 0 });
        f.advance(20000);
        require(acquire(f.mapping.memory().step_sample[0]) >= 200 &&
                  f.machine.snapshot().accepted_steps[0] == 1,
                "motor power loss froze MCU time");
    }
    void lost_step_and_gearing()
    {
        Fixture f{};
        f.command(Operation::Fault, { 1, std::to_underlying(Fault::LostStep) });
        for (unsigned i{}; i < 16; ++i) {
            f.arm(1);
            f.advance(20000);
        }
        const auto state{ f.machine.snapshot() };
        require(state.accepted_steps[1] == 16 && state.shaft_degrees[1] == 0,
                "lost step did not separate electrical pulses from shaft truth");
        Profile profile{};
        profile.ratio[2] = 2;
        Fixture geared{ profile };
        const double before{ geared.machine.snapshot().joints[2] };
        geared.command(Operation::Fault,
                       { 2, std::to_underlying(Fault::ShaftSlip), std::bit_cast<std::uint64_t>(360.0) });
        require(std::abs(geared.machine.snapshot().joints[2] - before - 0.625) < 1e-9, "Z gearing ignored");
    }
    void common_wiring()
    {
        Fixture f{};
        f.machine.enableTrace();
        f.command(Operation::GpioWrite, { ENABLE_PIN, 0 });
        f.command(Operation::GpioWrite, { DIR_PINS[0], 1 });
        f.advance(10000);
        f.command(Operation::GpioWrite, { STEP_PINS[0], 1 });
        f.advance(5000);
        f.command(Operation::GpioWrite, { STEP_PINS[0], 0 });
        require(f.machine.snapshot().accepted_steps[0] == 1, "GPIO STEP did not reach driver");
        f.command(Operation::PwmConfigure, { 0, STEP_PINS[0], 10000, 5000 });
        f.command(Operation::PwmStart, { 0, 2, 1 });
        f.advance(30000);
        require(f.machine.snapshot().accepted_steps[0] == 3, "PWM STEP did not reach driver");
        unsigned edges{};
        for (const auto& event : f.machine.trace())
            if (event.pin == STEP_PINS[0])
                ++edges;
        require(edges == 6, "trace omitted GPIO or PWM transitions");
        // An unused PWM channel must not default to driving PA0/STEP low.
        f.command(Operation::GpioWrite, { STEP_PINS[0], 1 });
        f.command(Operation::PwmStop, { 1 });
        require(acquire(f.mapping.memory().gpio[STEP_PINS[0]]) == 1, "unused PWM stop drove PA0");
    }
    void sealed_mapping()
    {
        Mapping original{};
        const int fd{ memfd_create("unsealed-simulation-test", MFD_CLOEXEC | MFD_ALLOW_SEALING) };
        require(fd >= 0 && ftruncate(fd, MAPPING_BYTES) == 0, "unsealed mapping fixture");
        require(pwrite(fd, &original.memory(), sizeof(SharedMemory), 0) == sizeof(SharedMemory),
                "mapping copy");
        bool rejected{};
        try {
            Mapping invalid{ fd };
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        close(fd);
        require(rejected, "accepted a mapping that the peer could truncate");
    }
    void cleanup()
    {
        Mapping mapping{};
        bool armed{};
        try {
            Machine machine{ mapping.memory() };
            const auto cmd{ [&](Operation op, std::array<std::uint64_t, 8> args) {
                return machine.execute(
                  { .operation = op, .session = mapping.memory().session, .arguments = args });
            } };
            cmd(Operation::GpioWrite, { ENABLE_PIN, 0 });
            cmd(Operation::PwmConfigure, { 0, pin(0, 8), 10000, 10000 });
            cmd(Operation::PwmStart, { 0, 0, 0 });
            require(acquire(mapping.memory().gpio[pin(0, 8)]) == 1, "PWM high fixture");
            armed = true;
            throw std::runtime_error{ "injected worker failure" };
        } catch (const std::runtime_error&) {
        }
        require(armed, "cleanup fixture did not reach an active output");
        require(acquire(mapping.memory().gpio[ENABLE_PIN]) == 1 &&
                  acquire(mapping.memory().gpio[pin(0, 8)]) == 0 && acquire(mapping.memory().pwm[0][1]) == 0,
                "exception cleanup left outputs active");
    }
}
int main()
try {
    timers();
    atomic_fault();
    power_and_reset();
    lost_step_and_gearing();
    common_wiring();
    sealed_mapping();
    cleanup();
    std::puts("PASS: atomic rejection, fractional timers, motor-power boundary, reset, gearing and exception "
              "cleanup");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
