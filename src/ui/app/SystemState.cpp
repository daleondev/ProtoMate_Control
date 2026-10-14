#include "SystemState.hpp"

#include <stdexcept>
#include <utility>

namespace ui
{
    SystemState::SystemState(std::shared_ptr<hal::device::ILed> red,
                             std::shared_ptr<hal::device::ILed> green)
      : m_red{ std::move(red) }, m_green{ std::move(green) }
    {
        if (!m_red || !m_green) { throw std::invalid_argument("system state requires red and green LEDs"); }
        update(Clock::now());
    }

    void SystemState::setLocal(app::State state, Clock::time_point now)
    {
        std::scoped_lock lock{ m_mutex };
        switch (state) {
            case app::State::Starting: break;
            case app::State::Ready: m_local.ready(); break;
            case app::State::Stopping: m_local.stopping(); break;
            case app::State::Stopped: m_local.stopped(); break;
            case app::State::Fault: m_local.fail(); break;
            default: m_local.fail(); break;
        }
        update(now);
    }

    void SystemState::setConnection(icc::ConnectionStatus status, Clock::time_point now)
    {
        std::scoped_lock lock{ m_mutex };
        if (status.state != m_connection.state) {
            m_connectedAt.reset();
            m_receivedAt.reset();
            m_snapshot.io.reset();
            if (status.state == icc::ConnectionState::Connected) { m_connectedAt = now; }
        }
        m_connection = status;
        if (status.state == icc::ConnectionState::Failed) { m_fault = true; }
        if (status.reason == icc::ConnectionReason::PeerStopped) {
            // An orderly goodbye is authoritative lifecycle information, not
            // evidence that the physical motors have actually stopped.
            m_snapshot.io = app::State::Stopped;
        }
        update(now);
    }

    void SystemState::receiveIo(app::State state, Clock::time_point now)
    {
        std::scoped_lock lock{ m_mutex };
        if (m_connection.state != icc::ConnectionState::Connected) { return; }
        if (state > app::State::Fault) { state = app::State::Fault; }
        m_snapshot.io = state;
        m_receivedAt = now;
        if (state == app::State::Fault) { m_fault = true; }
        update(now);
    }

    void SystemState::refresh(Clock::time_point now)
    {
        std::scoped_lock lock{ m_mutex };
        update(now);
    }

    SystemState::Snapshot SystemState::snapshot() const
    {
        std::scoped_lock lock{ m_mutex };
        return m_snapshot;
    }

    void SystemState::update(Clock::time_point now)
    {
        using enum app::State;
        m_snapshot.ui = m_local.get();
        const bool connected{ m_connection.state == icc::ConnectionState::Connected };
        m_snapshot.io_fresh = connected && m_receivedAt && now - *m_receivedAt < app::STATE_MAX_AGE;
        const auto last{ m_receivedAt ? m_receivedAt : m_connectedAt };
        const bool stopping{ m_snapshot.ui == Stopping || m_snapshot.ui == Stopped ||
                             m_snapshot.io == Stopping || m_snapshot.io == Stopped ||
                             m_connection.state == icc::ConnectionState::Stopping };
        if (connected && !stopping && last && now - *last >= app::STATE_MAX_AGE) { m_fault = true; }
        if (m_snapshot.ui == Fault) { m_fault = true; }

        if (m_fault) { m_snapshot.combined = Fault; }
        else if (m_snapshot.ui == Stopped) { m_snapshot.combined = Stopped; }
        else if (stopping) { m_snapshot.combined = Stopping; }
        else if (m_snapshot.ui == Ready && m_snapshot.io == Ready && m_snapshot.io_fresh) {
            m_snapshot.combined = Ready;
        } else { m_snapshot.combined = Starting; }

        // The GIGA has an RGB LED: red + green produces yellow; blue stays off.
        // Turn unwanted channels off first, avoiding a transient false green.
        const bool red{ m_snapshot.combined != Ready };
        const bool green{ m_snapshot.combined != Fault };
        if (!green) { m_green->turnOff(); }
        if (!red) { m_red->turnOff(); }
        if (red) { m_red->turnOn(); }
        if (green) { m_green->turnOn(); }
    }
}
