#pragma once
#include "ControllerProcess.hpp"
#include "Profile.hpp"
#include "simulation/SharedMemory.hpp"
#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>

namespace simulation
{
    class Machine;
    bool stop_requested() noexcept;
    void install_signal_handlers();
    struct SessionOptions
    {
        Profile profile{};
        std::filesystem::path media_directory{}, trace_path{};
    };
    class Session
    {
      public:
        Session(Session&&) = delete;
        auto operator=(Session&&) -> Session& = delete;

        explicit Session(SessionOptions options = {});
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        // Must precede start() and any SDL/thread initialization. The child only
        // performs async-signal-safe setup before executing the supplied ELF fd.
        void launch(int executable_fd, std::span<char*> arguments);
        void start();
        void stop() noexcept;
        bool running() const noexcept { return m_running.load(); }
        int childStatus() const noexcept { return m_childStatus.load(); }
        PlantSnapshot snapshot() const;
        const Profile& profile() const noexcept { return m_options.profile; }
        SharedMemory& memory() noexcept { return m_mapping.memory(); }

      private:
        void work(const std::stop_token& stop);
        void execute(Machine& machine, const std::stop_token& stop);
        struct ClockState
        {
            std::uint64_t next_wall{}, next_tick{};
        };
        struct PeerState
        {
            std::uint64_t wall{}, started{};
            bool ready{}, had_client{};
        };
        void serviceRegisters(Machine& /*machine*/);
        void advanceClock(Machine& /*machine*/, ClockState& /*clock*/, std::uint64_t wall, bool ready);
        void checkPeer(const Machine& /*machine*/, PeerState /*state*/);
        void publishSnapshot(const Machine& machine);
        const SessionOptions m_options;
        const MechanicalConfiguration m_configuration;
        Mapping m_mapping;
        ControllerProcess m_process;
        std::array<int, 4> m_events{ -1, -1, -1, -1 };
        std::atomic_bool m_running{};
        std::atomic_int m_childStatus{};
        mutable std::mutex m_snapshotMutex;
        PlantSnapshot m_snapshot{};
        std::jthread m_worker;
    };
}
