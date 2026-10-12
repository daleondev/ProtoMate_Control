#include "common/app/Application.hpp"
#include "common/icc/transport/InProcessTransport.hpp"
#include "io/app/Application.hpp"
#include "ui/app/Application.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <thread>
#include <array>
#include <unistd.h>

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
    static_assert(!app::Application<MissingJoin>);
    static_assert(!app::Application<WrongReturnType>);
    static_assert(!std::default_initializable<io::Application>);
    static_assert(!std::default_initializable<ui::Application>);

    template<typename Predicate>
    bool wait_until(Predicate predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        return predicate();
    }

    // Keep CLI input open and idle during lifecycle tests, independently of
    // CTest's redirected stdin. Its reader must still stop on peer/application stop.
    class ConsoleInput
    {
      public:
        ConsoleInput()
        {
            m_saved = ::dup(STDIN_FILENO);
            if (m_saved < 0 || ::pipe(m_pipe.data()) != 0) {
                throw std::runtime_error("cannot prepare test console");
            }
            if (::dup2(m_pipe[0], STDIN_FILENO) < 0) {
                throw std::runtime_error("cannot attach test console");
            }
            ::close(m_pipe[0]);
        }
        ~ConsoleInput()
        {
            ::dup2(m_saved, STDIN_FILENO);
            ::close(m_saved);
            if (m_pipe[1] >= 0) { ::close(m_pipe[1]); }
        }
        ConsoleInput(const ConsoleInput&) = delete;
        ConsoleInput& operator=(const ConsoleInput&) = delete;
        void close()
        {
            ::close(m_pipe[1]);
            m_pipe[1] = -1;
        }

      private:
        std::array<int, 2> m_pipe{ -1, -1 };
        int m_saved{};
    };

    struct Applications
    {
        ConsoleInput console;
        decltype(icc::InProcessTransport::pair()) endpoints{ icc::InProcessTransport::pair() };
        io::Application io{ *endpoints[0] };
        ui::Application ui{ *endpoints[1] };

        void start()
        {
            io.start();
            ui.start();
        }

        bool connected()
        {
            return wait_until([this] {
                return io.connectionStatus().state == icc::ConnectionState::Connected &&
                       ui.connectionStatus().state == icc::ConnectionState::Connected;
            });
        }
    };

    TEST(ApplicationLifecycle, StopDuringHandshakeDrainsWithoutPeer)
    {
        Applications apps;
        apps.io.start();
        apps.io.requestStop();
        apps.io.join();
        EXPECT_EQ(apps.io.connectionStatus().state, icc::ConnectionState::Stopped);
        EXPECT_THROW(apps.io.start(), std::logic_error);
    }

    TEST(ApplicationLifecycle, EmptyCatalogsKeepConnectionHealthyWhileApplicationsAreIdle)
    {
        Applications apps;
        apps.start();
        ASSERT_TRUE(apps.connected());
        // Longer than the 500 ms link-loss deadline: heartbeat traffic must
        // continue even with no application topics, services or actions.
        std::this_thread::sleep_for(650ms);
        EXPECT_EQ(apps.io.connectionStatus().state, icc::ConnectionState::Connected);
        EXPECT_EQ(apps.ui.connectionStatus().state, icc::ConnectionState::Connected);
    }

    TEST(ApplicationLifecycle, ConstructionAndDestructionNeedNoRunningContexts)
    {
        Applications apps;
        EXPECT_EQ(apps.io.connectionStatus().state, icc::ConnectionState::Stopped);
        EXPECT_EQ(apps.ui.connectionStatus().state, icc::ConnectionState::Stopped);
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
        EXPECT_EQ(apps.io.connectionStatus().state, icc::ConnectionState::Stopped);
        EXPECT_EQ(apps.ui.connectionStatus().state, icc::ConnectionState::Stopped);
    }

    TEST(ApplicationLifecycle, UiStopWakesIoEvenWhenMainIsJoiningIoFirst)
    {
        Applications apps;
        apps.start();
        ASSERT_TRUE(apps.connected());
        apps.ui.requestStop();
        apps.io.join();
        apps.ui.join();
        EXPECT_EQ(apps.io.connectionStatus().state, icc::ConnectionState::Stopped);
        EXPECT_EQ(apps.io.connectionStatus().reason, icc::ConnectionReason::PeerStopped);
    }

    TEST(ApplicationLifecycle, IoStopWakesUi)
    {
        Applications apps;
        apps.start();
        ASSERT_TRUE(apps.connected());
        apps.io.requestStop();
        apps.ui.join();
        apps.io.join();
        EXPECT_EQ(apps.ui.connectionStatus().reason, icc::ConnectionReason::PeerStopped);
    }

    TEST(ApplicationLifecycle, ConsoleEofStopsUiAndItsPeer)
    {
        Applications apps;
        apps.start();
        ASSERT_TRUE(apps.connected());
        apps.console.close();
        apps.ui.join();
        apps.io.join();
        EXPECT_EQ(apps.ui.connectionStatus().state, icc::ConnectionState::Stopped);
        EXPECT_EQ(apps.io.connectionStatus().reason, icc::ConnectionReason::PeerStopped);
    }

    TEST(ApplicationLifecycle, DisconnectStopsBothApplications)
    {
        Applications apps;
        apps.start();
        ASSERT_TRUE(apps.connected());
        apps.endpoints[0]->close();
        apps.io.join();
        apps.ui.join();
        EXPECT_EQ(apps.io.connectionStatus().state, icc::ConnectionState::Failed);
        EXPECT_EQ(apps.ui.connectionStatus().state, icc::ConnectionState::Failed);
    }

    TEST(ApplicationLifecycle, MissingPeerDoesNotBlockStartAndEventuallyStops)
    {
        Applications apps;
        apps.io.start();
        apps.io.join();
        EXPECT_EQ(apps.io.connectionStatus().state, icc::ConnectionState::Failed);
        EXPECT_EQ(apps.io.connectionStatus().reason, icc::ConnectionReason::HandshakeTimeout);
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
        EXPECT_NE(apps.io.connectionStatus().state, icc::ConnectionState::Connected);
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
