#include "common/Application.hpp"
#include "io/Application.hpp"
#include "ui/Application.hpp"
#include "p2p/transport/InProcessTransport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <thread>

namespace
{
    using namespace std::chrono_literals;

    struct MissingJoin
    {
        void start();
        void requestStop();
    };
    struct WrongReturnType
    {
        int start();
        void requestStop();
        void join();
    };
    static_assert(!application::Application<MissingJoin>);
    static_assert(!application::Application<WrongReturnType>);

    template<typename Predicate>
    bool wait_until(Predicate predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        return predicate();
    }

    struct Applications
    {
        decltype(p2p::InProcessTransport::pair()) endpoints{p2p::InProcessTransport::pair()};
        io::Application io{*endpoints[0]};
        ui::Application ui{*endpoints[1]};

        void start()
        {
            io.start();
            ui.start();
        }

        bool connected()
        {
            return wait_until([this] {
                return io.connectionStatus().state == p2p::ConnectionState::Connected &&
                       ui.connectionStatus().state == p2p::ConnectionState::Connected;
            });
        }
    };

    TEST(ApplicationLifecycle, ConstructionAndDestructionNeedNoRunningContexts)
    {
        Applications apps;
        EXPECT_EQ(apps.io.connectionStatus().state, p2p::ConnectionState::Stopped);
        EXPECT_EQ(apps.ui.connectionStatus().state, p2p::ConnectionState::Stopped);
        apps.io.join();
        apps.ui.join();
    }

    TEST(ApplicationLifecycle, IndependentRunnersHandshakeAndBothStop)
    {
        Applications apps;
        apps.start();
        ASSERT_TRUE(apps.connected());
        apps.io.requestStop();
        apps.ui.requestStop();
        apps.io.join();
        apps.ui.join();
        EXPECT_EQ(apps.io.connectionStatus().state, p2p::ConnectionState::Stopped);
        EXPECT_EQ(apps.ui.connectionStatus().state, p2p::ConnectionState::Stopped);
    }

    TEST(ApplicationLifecycle, UiStopWakesIoEvenWhenMainIsJoiningIoFirst)
    {
        Applications apps;
        apps.start();
        ASSERT_TRUE(apps.connected());
        apps.ui.requestStop();
        apps.io.join();
        apps.ui.join();
        EXPECT_EQ(apps.io.connectionStatus().state, p2p::ConnectionState::Stopped);
        EXPECT_EQ(apps.io.connectionStatus().reason, p2p::ConnectionReason::PeerStopped);
    }

    TEST(ApplicationLifecycle, IoStopWakesUi)
    {
        Applications apps;
        apps.start();
        ASSERT_TRUE(apps.connected());
        apps.io.requestStop();
        apps.ui.join();
        apps.io.join();
        EXPECT_EQ(apps.ui.connectionStatus().reason, p2p::ConnectionReason::PeerStopped);
    }

    TEST(ApplicationLifecycle, DisconnectStopsBothApplications)
    {
        Applications apps;
        apps.start();
        ASSERT_TRUE(apps.connected());
        apps.endpoints[0]->close();
        apps.io.join();
        apps.ui.join();
        EXPECT_EQ(apps.io.connectionStatus().state, p2p::ConnectionState::Failed);
        EXPECT_EQ(apps.ui.connectionStatus().state, p2p::ConnectionState::Failed);
    }

    TEST(ApplicationLifecycle, MissingPeerDoesNotBlockStartAndEventuallyStops)
    {
        Applications apps;
        apps.io.start();
        apps.io.join();
        EXPECT_EQ(apps.io.connectionStatus().state, p2p::ConnectionState::Failed);
        EXPECT_EQ(apps.io.connectionStatus().reason, p2p::ConnectionReason::HandshakeTimeout);
    }

    TEST(ApplicationLifecycle, ImmediateStopCanBeRepeated)
    {
        Applications apps;
        apps.start();
        apps.io.requestStop();
        apps.ui.requestStop();
        apps.io.requestStop();
        apps.io.join();
        apps.ui.join();
        apps.io.join();
        EXPECT_NE(apps.io.connectionStatus().state, p2p::ConnectionState::Connected);
    }

    TEST(ApplicationLifecycle, StartIsOneShotAndCannotFollowStopBeforeStart)
    {
        Applications apps;
        apps.io.requestStop();
        EXPECT_THROW(apps.io.start(), std::logic_error);
        apps.ui.start();
        EXPECT_THROW(apps.ui.start(), std::logic_error);
        apps.ui.requestStop();
        apps.ui.join();
        EXPECT_THROW(apps.ui.start(), std::logic_error);
    }

    TEST(ApplicationLifecycle, DestructionDrainsRunnersWithoutExplicitStop)
    {
        for (int iteration = 0; iteration != 5; ++iteration) {
            Applications apps;
            apps.start();
            ASSERT_TRUE(apps.connected());
        }
    }
}
