#pragma once
#include "Machine.hpp"
#include <atomic>
#include <mutex>
#include <thread>

namespace simulation
{
    bool stopRequested() noexcept;
    void installSignalHandlers();
    class Session
    {
      public:
        Session();
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        void launch(const char* executable, std::span<char*> arguments);
        void start();
        void stop() noexcept;
        bool running() const noexcept { return m_running.load(); }
        int childStatus() const noexcept { return m_child_status.load(); }
        PlantSnapshot snapshot() const;
        SharedMemory& memory() noexcept { return m_mapping.memory(); }

      private:
        void work(std::stop_token stop);
        Mapping m_mapping;
        std::array<int, 4> m_events{ -1, -1, -1, -1 };
        int m_child{ -1 };
        std::atomic_bool m_running{};
        std::atomic_int m_child_status{};
        mutable std::mutex m_snapshot_mutex;
        PlantSnapshot m_snapshot{};
        std::jthread m_worker;
    };
}
