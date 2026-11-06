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
    inline constexpr unsigned pin(unsigned port, unsigned number) { return port * 16U + number; }
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
        using Configuration = std::array<double, 20>;
        static Configuration configuration(const Profile& profile);
        explicit Machine(SharedMemory& memory,
                         Profile profile = {},
                         const std::filesystem::path& media = {},
                         std::optional<Configuration> configuration = {});
        ~Machine() { disconnect(); }
        Packet execute(const Packet& request);
        void advance(std::uint64_t nanoseconds);
        void publish();
        PlantSnapshot snapshot() const;
        void disconnect() noexcept;
        void enableTrace() noexcept { m_trace_enabled = true; }
        std::span<const TraceEvent> trace() const noexcept { return { m_trace.data(), m_trace_count }; }
        void clearTrace() noexcept { m_trace_count = 0; }
        bool traceOverflow() const noexcept { return m_trace_overflow; }
        bool interruptPending() const noexcept { return m_interrupt != 0; }
        unsigned takeInterrupt() noexcept { return std::exchange(m_interrupt, 0U); }
        bool leased() const noexcept { return m_lease; }
        std::uint64_t time() const noexcept { return m_time; }
        const Profile& profile() const noexcept { return m_profile; }

      private:
        void edge(const hal::util::SimulatedStepHardware::Edge& edge);
        void output(unsigned pin, bool high, std::uint64_t time);
        void stepEdge(unsigned axis, bool high, std::uint64_t time);
        void input(unsigned pin, bool value);
        void sensors();
        void record(std::uint64_t time, unsigned pin, bool high) noexcept;
        std::array<TraceEvent, 8192> m_trace{};
        std::size_t m_trace_count{};
        bool m_trace_enabled{}, m_trace_overflow{};
        SharedMemory& m_memory;
        Profile m_profile;
        std::optional<Configuration> m_configuration{};
        Kinematics m_kinematics;
        ContactSensors m_contacts;
        hal::util::SimulatedStepHardware m_step;
        hal::device::Tmc2209Model m_tmc;
        std::array<double, 3> m_shaft{}, m_electrical{};
        std::array<std::int64_t, 3> m_steps{};
        std::array<std::uint64_t, 3> m_rise{}, m_direction_time{};
        std::array<bool, 3> m_reference{}, m_pending_step{};
        std::array<int, 3> m_pending_direction{};
        std::array<unsigned, 3> m_pending_resolution{};
        std::array<std::uint64_t, 3> m_rejected_pulses{};
        std::array<Fault, 3> m_fault{};
        std::array<std::uint32_t, 176> m_outputs{};
        std::array<bool, 176> m_is_output{};
        std::uint64_t m_time{}, m_step_clock{}, m_edge_epoch{}, m_sequence{}, m_last_request{};
        std::int64_t m_encoder_count{}, m_encoder_last{};
        bool m_encoder_running{};
        unsigned m_interrupt{};
        bool m_power{ true }, m_lease{}, m_disconnected{};
        Packet m_last_reply{}, m_last_command{};
        Media m_media;
        Peripherals m_peripherals;
    };
}
