#pragma once
#include "ContactSensors.hpp"
#include "Kinematics.hpp"
#include "Media.hpp"
#include "Peripherals.hpp"
#include "Profile.hpp"
#include "SimulatedStepHardware.hpp"
#include "Tmc2209Model.hpp"
#include "simulation/SharedMemory.hpp"
#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace simulation
{
    inline constexpr unsigned PINS_PER_PORT{ 16 };
    constexpr unsigned pin(unsigned port, unsigned number) { return (port * PINS_PER_PORT) + number; }
    inline constexpr std::array<unsigned, 3> STEP_PINS{ pin(0, 0), pin(0, 2), pin(0, 3) };
    inline constexpr std::array<unsigned, 3> DIR_PINS{ pin(9, 8), pin(3, 13), pin(7, 13) };
    inline constexpr std::array<unsigned, 3> REF_PINS{ pin(4, 4), pin(4, 5), pin(1, 2) };
    inline constexpr std::array<unsigned, 3> ALARM_PINS{ pin(1, 1), pin(2, 3), pin(2, 0) };
    inline constexpr unsigned ENABLE_PIN{ pin(2, 7) };
    inline constexpr std::array<unsigned, 2> INDEX_PINS{ pin(1, 8), pin(1, 9) };
    struct TraceEvent
    {
        std::uint64_t time_ns{};
        unsigned pin{};
        bool high{};
    };
    class Machine
    {
      public:
        Machine(const Machine&) = delete;
        auto operator=(const Machine&) -> Machine& = delete;
        Machine(Machine&&) = delete;
        auto operator=(Machine&&) -> Machine& = delete;

        using Configuration = MechanicalConfiguration;
        static Configuration configuration(const Profile& profile);
        explicit Machine(SharedMemory& memory,
                         Profile profile = {},
                         const std::filesystem::path& media = {},
                         std::optional<Configuration> configuration = {});
        ~Machine() { disconnect(); }
        Packet execute(const Packet& q);
        void advance(std::uint64_t nanoseconds);
        void publish();
        PlantSnapshot snapshot() const;
        void disconnect() noexcept;
        void enableTrace() noexcept { m_traceEnabled = true; }
        std::span<const TraceEvent> trace() const noexcept { return { m_trace.data(), m_traceCount }; }
        void clearTrace() noexcept { m_traceCount = 0; }
        bool traceOverflow() const noexcept { return m_traceOverflow; }
        bool interruptPending() const noexcept { return m_interrupt != 0; }
        unsigned takeInterrupt() noexcept { return std::exchange(m_interrupt, 0U); }
        bool leased() const noexcept { return m_lease; }
        std::uint64_t time() const noexcept { return m_time; }
        const Profile& profile() const noexcept { return m_profile; }

      private:
        static constexpr std::size_t TRACE_CAPACITY{ 8192 };
        void faultCommand(const Packet& q, Packet& r);
        void uartCommand(const Packet& q, Packet& r);
        bool systemCommand(const Packet& q, Packet& r);
        bool driverCommand(const Packet& q, Packet& r);
        bool validFuturePublication(const Packet& /*q*/, std::span<const std::byte> /*bytes*/) const;
        void retargetCommand(const Packet& q, Packet& r);
        bool publicationCommand(const Packet& q, Packet& r);
        bool gpioCommand(const Packet& q, Packet& r);
        bool stepCommand(const Packet& q, Packet& r);
        void edge(const hal::util::SimulatedStepHardware::Edge& edge);
        void output(unsigned index, bool high, std::uint64_t time);
        void stepEdge(unsigned axis, bool high, std::uint64_t time);
        void input(unsigned index, bool value);
        void sensors();
        void record(std::uint64_t time, unsigned index, bool high) noexcept;
        std::array<TraceEvent, TRACE_CAPACITY> m_trace{};
        std::size_t m_traceCount{};
        bool m_traceEnabled{}, m_traceOverflow{};
        SharedMemory& m_memory;
        Profile m_profile;
        std::optional<Configuration> m_configuration{};
        Kinematics m_kinematics;
        ContactSensors m_contacts;
        hal::util::SimulatedStepHardware m_step;
        hal::device::Tmc2209Model m_tmc;
        std::array<double, 3> m_shaft{}, m_electrical{};
        std::array<std::int64_t, 3> m_steps{};
        std::array<std::uint64_t, 3> m_rise{}, m_directionTime{};
        std::array<bool, 3> m_reference{}, m_pendingStep{};
        std::array<int, 3> m_pendingDirection{};
        std::array<unsigned, 3> m_pendingResolution{};
        std::array<std::uint64_t, 3> m_rejectedPulses{};
        std::array<Fault, 3> m_fault{};
        std::array<std::uint32_t, GPIO_COUNT> m_outputs{};
        std::array<bool, GPIO_COUNT> m_isOutput{};
        std::uint64_t m_time{}, m_stepClock{}, m_edgeEpoch{}, m_sequence{}, m_lastRequest{};
        std::int64_t m_encoderCount{}, m_encoderLast{};
        bool m_encoderRunning{};
        unsigned m_interrupt{};
        bool m_power{ true }, m_lease{}, m_disconnected{};
        Packet m_lastReply{}, m_lastCommand{};
        Media m_media;
        Peripherals m_peripherals;
    };
}
