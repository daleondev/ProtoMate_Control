#pragma once

#include "common/app/State.hpp"
#include "common/icc/bridge/ConnectionHealth.hpp"
#include "hal/devices/itf/ILed.hpp"

#include <memory>
#include <mutex>
#include <optional>

namespace ui
{
    // UI owns the aggregate and physical indicators. All entry points are
    // thread-safe; timestamps belong to UI's clock, never IO's clock.
    class SystemState final
    {
      public:
        using Clock = std::chrono::steady_clock;
        struct Snapshot
        {
            app::State ui{ app::State::Starting };
            std::optional<app::State> io;
            app::State combined{ app::State::Starting };
            bool io_fresh{};
        };

        SystemState(std::shared_ptr<hal::device::ILed> red, std::shared_ptr<hal::device::ILed> green);
        void setLocal(app::State state, Clock::time_point now = Clock::now());
        void setConnection(icc::ConnectionStatus status, Clock::time_point now = Clock::now());
        void receiveIo(app::State state, Clock::time_point now = Clock::now());
        void refresh(Clock::time_point now = Clock::now());
        [[nodiscard]] Snapshot snapshot() const;

      private:
        void update(Clock::time_point now);

        mutable std::mutex m_mutex;
        app::ControllerState m_local;
        Snapshot m_snapshot;
        icc::ConnectionStatus m_connection;
        std::optional<Clock::time_point> m_connectedAt, m_receivedAt;
        bool m_fault{}; // Latched until application restart, including stale IO telemetry.
        std::shared_ptr<hal::device::ILed> m_red, m_green;
    };
}
