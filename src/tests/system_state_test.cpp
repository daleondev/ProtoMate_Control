#include "common/app/State.hpp"
#include "ui/app/SystemState.hpp"

#include <gtest/gtest.h>

namespace
{
    using enum app::State;
    using namespace std::chrono_literals;

    class Led final : public hal::device::ILed
    {
      public:
        void turnOn() noexcept override { m_on = true; }
        void turnOff() noexcept override { m_on = false; }
        void toggle() noexcept override { m_on = !m_on; }
        bool isOn() const noexcept override { return m_on; }
      private:
        bool m_on{};
    };

    struct SystemStateTest : testing::Test
    {
        std::shared_ptr<Led> red{ std::make_shared<Led>() }, green{ std::make_shared<Led>() };
        ui::SystemState system{ red, green };
        const ui::SystemState::Clock::time_point now{ ui::SystemState::Clock::now() };

        void connect()
        {
            system.setLocal(Ready, now);
            system.setConnection({ icc::ConnectionState::Connected }, now);
        }
        void expectColor(bool r, bool g)
        {
            EXPECT_EQ(red->isOn(), r);
            EXPECT_EQ(green->isOn(), g);
        }
    };

    TEST(ControllerState, ShutdownAndFaultCannotBeOverwrittenByLateStartup)
    {
        app::ControllerState state;
        EXPECT_EQ(state.get(), Starting);
        state.stopping();
        state.ready();
        EXPECT_EQ(state.get(), Stopping);
        state.stopped();
        state.ready();
        state.stopping();
        EXPECT_EQ(state.get(), Stopped);
        state.fail();
        state.stopping();
        state.stopped();
        state.ready();
        EXPECT_EQ(state.get(), Fault);
    }

    TEST_F(SystemStateTest, YellowUntilBothControllersAndLinkAreReady)
    {
        EXPECT_EQ(system.snapshot().combined, Starting);
        EXPECT_FALSE(system.snapshot().io);
        expectColor(true, true);
        // Data received outside an active session cannot make us ready.
        system.receiveIo(Ready, now);
        EXPECT_FALSE(system.snapshot().io);
        connect();
        EXPECT_EQ(system.snapshot().combined, Starting);
        system.receiveIo(Starting, now);
        EXPECT_EQ(system.snapshot().combined, Starting);
        system.receiveIo(Ready, now);
        EXPECT_EQ(system.snapshot().combined, Ready);
        EXPECT_TRUE(system.snapshot().io_fresh);
        expectColor(false, true);
    }

    TEST_F(SystemStateTest, FreshIoAloneDoesNotMeanUiIsReady)
    {
        system.setConnection({ icc::ConnectionState::Connected }, now);
        system.receiveIo(Ready, now);
        EXPECT_EQ(system.snapshot().combined, Starting);
        system.setLocal(Ready, now);
        EXPECT_EQ(system.snapshot().combined, Ready);
    }

    TEST_F(SystemStateTest, ConnectedPeerMustActuallyPublishItsState)
    {
        connect();
        system.refresh(now + 499ms);
        EXPECT_EQ(system.snapshot().combined, Starting);
        system.refresh(now + 500ms);
        EXPECT_EQ(system.snapshot().combined, Fault);
        expectColor(true, false);
    }

    TEST_F(SystemStateTest, StaleStateCannotRemainGreenOrClearItsFault)
    {
        connect();
        system.receiveIo(Ready, now);
        system.refresh(now + 499ms);
        EXPECT_EQ(system.snapshot().combined, Ready);
        system.refresh(now + 500ms);
        EXPECT_FALSE(system.snapshot().io_fresh);
        EXPECT_EQ(system.snapshot().combined, Fault);
        expectColor(true, false);
        system.receiveIo(Ready, now + 501ms);
        EXPECT_TRUE(system.snapshot().io_fresh);
        EXPECT_EQ(system.snapshot().combined, Fault);
        system.setLocal(Stopped, now + 502ms);
        EXPECT_EQ(system.snapshot().combined, Fault);
    }

    TEST_F(SystemStateTest, RemoteFaultIsLatchedThroughGoodbyeAndCleanup)
    {
        connect();
        system.receiveIo(Fault, now);
        EXPECT_EQ(system.snapshot().ui, Ready);
        EXPECT_EQ(system.snapshot().io, Fault);
        EXPECT_EQ(system.snapshot().combined, Fault);
        system.setConnection({ icc::ConnectionState::Stopped, icc::ConnectionReason::PeerStopped }, now);
        system.setLocal(Stopped, now);
        EXPECT_EQ(system.snapshot().combined, Fault);
        expectColor(true, false);
    }

    TEST_F(SystemStateTest, LocalFaultAndInvalidRemoteStateAreRed)
    {
        system.setLocal(Fault, now);
        system.setLocal(Ready, now);
        EXPECT_EQ(system.snapshot().ui, Fault);
        expectColor(true, false);
        ui::SystemState other{ red, green };
        other.setConnection({ icc::ConnectionState::Connected }, now);
        other.receiveIo(static_cast<app::State>(255), now);
        EXPECT_EQ(other.snapshot().io, Fault);
        EXPECT_EQ(other.snapshot().combined, Fault);
        expectColor(true, false);
    }

    TEST_F(SystemStateTest, LinkFailureInvalidatesRemoteStateAndStaysRed)
    {
        connect();
        system.receiveIo(Ready, now);
        system.setConnection({ icc::ConnectionState::Failed, icc::ConnectionReason::HeartbeatTimeout }, now);
        EXPECT_FALSE(system.snapshot().io);
        EXPECT_FALSE(system.snapshot().io_fresh);
        EXPECT_EQ(system.snapshot().combined, Fault);
        expectColor(true, false);
        system.setLocal(Stopped, now);
        system.setConnection({ icc::ConnectionState::Stopped }, now);
        EXPECT_EQ(system.snapshot().combined, Fault);
    }

    TEST_F(SystemStateTest, OrderlyShutdownIsYellowAndDoesNotBecomeStaleFault)
    {
        connect();
        system.receiveIo(Ready, now);
        system.setLocal(Stopping, now);
        EXPECT_EQ(system.snapshot().combined, Stopping);
        expectColor(true, true);
        system.refresh(now + 2s);
        EXPECT_EQ(system.snapshot().combined, Stopping);
        system.setConnection({ icc::ConnectionState::Stopped, icc::ConnectionReason::PeerStopped }, now + 2s);
        system.setLocal(Stopped, now + 2s);
        EXPECT_EQ(system.snapshot().combined, Stopped);
        EXPECT_EQ(system.snapshot().io, Stopped);
        expectColor(true, true);
    }
}
