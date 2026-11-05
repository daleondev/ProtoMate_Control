#include "../Machine.hpp"
#include "hal/devices/util/Tmc2209Protocol.hpp"
#include <cmath>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <limits>
#include <sys/mman.h>
#include <unistd.h>

namespace
{
    using namespace simulation;
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
        void writeRegister(unsigned address, unsigned reg, std::uint32_t value)
        {
            Packet request{ .operation = Operation::Uart,
                            .size = 8,
                            .sequence = ++sequence,
                            .session = mapping.memory().session };
            std::array<std::uint8_t, 8> bytes{ 5,
                                               static_cast<std::uint8_t>(address),
                                               static_cast<std::uint8_t>(reg | 0x80U) };
            for (unsigned i{}; i < 4; ++i) {
                bytes[i + 3] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
            }
            for (unsigned i{}; i < 7; ++i) {
                auto byte{ bytes[i] };
                for (unsigned bit{}; bit < 8; ++bit) {
                    const bool feedback{ bool((bytes[7] >> 7U) ^ (byte & 1U)) };
                    bytes[7] = static_cast<std::uint8_t>(bytes[7] << 1U);
                    if (feedback) {
                        bytes[7] ^= 7;
                    }
                    byte >>= 1U;
                }
            }
            for (unsigned i{}; i < bytes.size(); ++i) {
                request.data[i] = std::byte{ bytes[i] };
            }
            ASSERT_TRUE((!machine.execute(request).status)) << "TMC register write";
        }
        void advance(std::uint64_t ns)
        {
            const auto end{ machine.time() + ns };
            while (machine.time() < end) {
                machine.takeInterrupt();
                const auto before{ machine.time() };
                machine.advance(end - before);
                ASSERT_TRUE((machine.time() > before)) << "device time failed to progress";
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
            ASSERT_TRUE((
              command(
                Operation::Publish,
                { axis, 0, 0, 1, checksum(std::span{ shared.bulk[0] }.first(sizeof(hal::util::StepBuffer))) })
                .status == 0))
              << "pulse buffer publication";
            ASSERT_TRUE((command(Operation::Arm, { axis, 100, 2 }).status == 0)) << "pulse arm";
        }
    };
    TEST(SimulationRegression, InvalidAndFractionalTimerConfiguration)
    {
        Fixture f{};
        auto& timer{ f.mapping.memory().timers[5] };
        ASSERT_TRUE((!f.command(Operation::TimerConfigure, { 5, 0, 240000000, 239, 999 }).status))
          << "configure";
        ASSERT_TRUE((!f.command(Operation::TimerStart, { 5, 1 }).status)) << "timer start";
        const auto original{ timer };
        for (const auto& args : { std::array<std::uint64_t, 8>{ 5, 0, 0, 239, 999 },
                                  std::array<std::uint64_t, 8>{ 5, 0, UINT64_MAX, 239, 999 },
                                  std::array<std::uint64_t, 8>{ 5, 0, 240000000, UINT64_MAX, 999 },
                                  std::array<std::uint64_t, 8>{ 5, 99, 10 } }) {
            ASSERT_TRUE((f.command(Operation::TimerConfigure, args).status == EINVAL))
              << "invalid timer accepted";
            ASSERT_TRUE((timer == original)) << "invalid timer mutated registers";
        }
        ASSERT_NO_FATAL_FAILURE(f.advance(1000000));
        ASSERT_TRUE((timer[6] == 1)) << "timer stopped after rejected configure";
        // 3 Hz / 2 is 1.5 Hz, not the 1 Hz produced by early integer division.
        f.command(Operation::TimerStop, { 5 });
        f.command(Operation::TimerConfigure, { 5, 0, 3, 1, 99 });
        f.command(Operation::TimerConfigure, { 5, 1, 0 });
        f.command(Operation::TimerStart, { 5, 0 });
        ASSERT_NO_FATAL_FAILURE(f.advance(2000000000));
        ASSERT_TRUE((timer[0] == 3)) << "fractional divider frequency was truncated";
        f.command(Operation::TimerConfigure, { 5, 3, 1 });
        f.command(Operation::TimerStart, { 5, 1 });
        ASSERT_NO_FATAL_FAILURE(f.advance(1000000000));
        ASSERT_TRUE((timer[0] <= 1)) << "counter/reload normalization";
    }
    TEST(SimulationRegression, InvalidFaultIsAtomic)
    {
        Fixture f{};
        ASSERT_TRUE((!f.command(Operation::Fault, { 1, std::to_underlying(Fault::DriverAlarm) }).status))
          << "alarm";
        const auto pose{ f.machine.snapshot() };
        ASSERT_TRUE((f.command(Operation::Fault,
                               { 1,
                                 std::to_underlying(Fault::ShaftSlip),
                                 std::bit_cast<std::uint64_t>(std::numeric_limits<double>::quiet_NaN()) })
                       .status == EINVAL))
          << "nonfinite slip accepted";
        ASSERT_TRUE((f.machine.snapshot().faults == pose.faults &&
                     acquire(f.mapping.memory().gpio[ALARM_PINS[1]]) == 1))
          << "rejected slip cleared existing alarm";
        ASSERT_TRUE((f.machine.snapshot().shaft_degrees == pose.shaft_degrees))
          << "rejected slip moved mechanism";
    }
    TEST(SimulationRegression, PowerLossAndResetDoNotInventSteps)
    {
        Fixture f{};
        ASSERT_NO_FATAL_FAILURE(f.arm());
        ASSERT_NO_FATAL_FAILURE(f.advance(10000));
        ASSERT_TRUE((acquire(f.mapping.memory().gpio[STEP_PINS[0]]) == 1)) << "rising edge fixture";
        f.command(Operation::Power, { 0 });
        ASSERT_TRUE((acquire(f.mapping.memory().step_sample[1]) == 1 && f.machine.snapshot().enabled))
          << "power fault changed controller timer or enable GPIO";
        f.command(Operation::Power, { 1 });
        ASSERT_NO_FATAL_FAILURE(f.advance(10000));
        ASSERT_TRUE((f.machine.snapshot().accepted_steps[0] == 0))
          << "power restoration accepted a partial pulse";
        ASSERT_NO_FATAL_FAILURE(f.arm());
        ASSERT_NO_FATAL_FAILURE(f.advance(20000));
        ASSERT_TRUE((f.machine.snapshot().accepted_steps[0] == 1)) << "full pulse after power restoration";
        ASSERT_NO_FATAL_FAILURE(f.arm());
        ASSERT_NO_FATAL_FAILURE(f.advance(10000));
        f.command(Operation::Reset);
        ASSERT_TRUE((acquire(f.mapping.memory().gpio[STEP_PINS[0]]) == 0)) << "reset left STEP high";
        ASSERT_TRUE((f.machine.snapshot().accepted_steps[0] == 1)) << "reset invented a mechanical step";
        ASSERT_NO_FATAL_FAILURE(f.arm());
        f.command(Operation::Power, { 0 });
        ASSERT_NO_FATAL_FAILURE(f.advance(20000));
        ASSERT_TRUE(
          (acquire(f.mapping.memory().step_sample[0]) >= 200 && f.machine.snapshot().accepted_steps[0] == 1))
          << "motor power loss froze MCU time";
    }
    TEST(SimulationRegression, LostStepsAndGearing)
    {
        Fixture f{};
        f.command(Operation::Fault, { 1, std::to_underlying(Fault::LostStep) });
        for (unsigned i{}; i < 16; ++i) {
            ASSERT_NO_FATAL_FAILURE(f.arm(1));
            ASSERT_NO_FATAL_FAILURE(f.advance(20000));
        }
        const auto state{ f.machine.snapshot() };
        ASSERT_TRUE((state.accepted_steps[1] == 16 && state.shaft_degrees[1] == 0))
          << "lost step did not separate electrical pulses from shaft truth";
        Profile profile{};
        profile.ratio[2] = 2;
        Fixture geared{ profile };
        const double before{ geared.machine.snapshot().joints[2] };
        geared.command(Operation::Fault,
                       { 2, std::to_underlying(Fault::ShaftSlip), std::bit_cast<std::uint64_t>(360.0) });
        ASSERT_TRUE((std::abs(geared.machine.snapshot().joints[2] - before - 0.625) < 1e-9))
          << "Z gearing ignored";
    }
    TEST(SimulationRegression, TmcMicrostepSelection)
    {
        using namespace hal::device::util::tmc2209;
        Fixture f;
        const auto pulse{ [&](unsigned axis) {
            const auto before{ f.machine.snapshot().shaft_degrees[axis] };
            f.arm(axis);
            f.advance(20000);
            return f.machine.snapshot().shaft_degrees[axis] - before;
        } };
        ASSERT_TRUE((std::abs(pulse(1) - 1.8 / 8) < 1e-9)) << "M2 reset must use MS2/MS1=00 (1/8)";
        ASSERT_TRUE((std::abs(pulse(2) - 1.8 / 32) < 1e-9)) << "M3 reset must use MS2/MS1=01 (1/32)";
        ASSERT_NO_FATAL_FAILURE(f.writeRegister(0, CHOPCONF, CHOPCONF_RESET | (4U << MRES_SHIFT)));
        ASSERT_TRUE((std::abs(pulse(1) - 1.8 / 8) < 1e-9)) << "MRES changed pin-selected resolution";
        ASSERT_NO_FATAL_FAILURE(f.writeRegister(0, 0x00, GCONF_RESET | GCONF_MSTEP_REG_SELECT));
        ASSERT_TRUE((std::abs(pulse(1) - 1.8 / 16) < 1e-9)) << "UART-selected MRES not applied";
        ASSERT_TRUE((std::abs(pulse(2) - 1.8 / 32) < 1e-9)) << "M2 UART selection changed M3";
        ASSERT_NO_FATAL_FAILURE(f.writeRegister(0, 0x00, GCONF_RESET));
        ASSERT_TRUE((std::abs(pulse(1) - 1.8 / 8) < 1e-9)) << "return to pin selection ignored";

        hal::device::Tmc2209Model driver;
        driver.setRegister(0, 0, GCONF_RESET | GCONF_MSTEP_REG_SELECT);
        for (unsigned exponent{}; exponent <= 8; ++exponent) {
            driver.setRegister(0, CHOPCONF, CHOPCONF_RESET | (exponent << MRES_SHIFT));
            ASSERT_TRUE((driver.microsteps(0) == (256U >> exponent))) << "legal MRES setting";
        }
        driver.reset(0);
        ASSERT_TRUE((driver.microsteps(0) == 8)) << "driver reset did not restore pin control";
    }
    TEST(SimulationRegression, GpioAndPwmShareStepWiring)
    {
        Fixture f{};
        f.machine.enableTrace();
        f.command(Operation::GpioWrite, { ENABLE_PIN, 0 });
        f.command(Operation::GpioWrite, { DIR_PINS[0], 1 });
        ASSERT_NO_FATAL_FAILURE(f.advance(10000));
        f.command(Operation::GpioWrite, { STEP_PINS[0], 1 });
        ASSERT_NO_FATAL_FAILURE(f.advance(5000));
        f.command(Operation::GpioWrite, { STEP_PINS[0], 0 });
        ASSERT_TRUE((f.machine.snapshot().accepted_steps[0] == 1)) << "GPIO STEP did not reach driver";
        f.command(Operation::PwmConfigure, { 0, STEP_PINS[0], 10000, 5000 });
        f.command(Operation::PwmStart, { 0, 2, 1 });
        ASSERT_NO_FATAL_FAILURE(f.advance(30000));
        ASSERT_TRUE((f.machine.snapshot().accepted_steps[0] == 3)) << "PWM STEP did not reach driver";
        unsigned edges{};
        for (const auto& event : f.machine.trace())
            if (event.pin == STEP_PINS[0])
                ++edges;
        ASSERT_TRUE((edges == 6)) << "trace omitted GPIO or PWM transitions";
        // An unused PWM channel must not default to driving PA0/STEP low.
        f.command(Operation::GpioWrite, { STEP_PINS[0], 1 });
        f.command(Operation::PwmStop, { 1 });
        ASSERT_TRUE((acquire(f.mapping.memory().gpio[STEP_PINS[0]]) == 1)) << "unused PWM stop drove PA0";
    }
    TEST(SimulationRegression, RejectsUnsealedMapping)
    {
        Mapping original{};
        const int fd{ memfd_create("unsealed-simulation-test", MFD_CLOEXEC | MFD_ALLOW_SEALING) };
        ASSERT_TRUE((fd >= 0 && ftruncate(fd, MAPPING_BYTES) == 0)) << "unsealed mapping fixture";
        ASSERT_TRUE((pwrite(fd, &original.memory(), sizeof(SharedMemory), 0) == sizeof(SharedMemory)))
          << "mapping copy";
        bool rejected{};
        try {
            Mapping invalid{ fd };
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        close(fd);
        ASSERT_TRUE((rejected)) << "accepted a mapping that the peer could truncate";
    }
    TEST(SimulationRegression, ExceptionDisablesOutputs)
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
            ASSERT_TRUE((acquire(mapping.memory().gpio[pin(0, 8)]) == 1)) << "PWM high fixture";
            armed = true;
            throw std::runtime_error{ "injected worker failure" };
        } catch (const std::runtime_error&) {
        }
        ASSERT_TRUE((armed)) << "cleanup fixture did not reach an active output";
        ASSERT_TRUE((acquire(mapping.memory().gpio[ENABLE_PIN]) == 1 &&
                     acquire(mapping.memory().gpio[pin(0, 8)]) == 0 &&
                     acquire(mapping.memory().pwm[0][1]) == 0))
          << "exception cleanup left outputs active";
    }
}
