#include "../Session.hpp"
#include <cstdlib>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <string_view>
#include <unistd.h>

namespace
{
    class SimulationSession : public testing::TestWithParam<const char*>
    {
    };

    TEST_P(SimulationSession, EarlyIpcClosePreservesExitStatusAndDisablesPlant)
    {
        const std::string_view scenario{ GetParam() };
        simulation::Session session{ { .trace_path = scenario == "trace" ? "/dev/full" : "" } };
        const int fd{ open("/proc/self/exe", O_RDONLY | O_CLOEXEC) };
        ASSERT_GE(fd, 0);
        char mode[]{ "--session-child" };
        std::array arguments{ mode, const_cast<char*>(GetParam()) };
        try {
            session.launch(fd, arguments);
        } catch (...) {
            close(fd);
            throw;
        }
        close(fd);
        session.start();
        const auto deadline{ std::chrono::steady_clock::now() + std::chrono::seconds{ 5 } };
        while (session.running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds{ 10 });
        ASSERT_FALSE(session.running()) << "Session failed to reap its child within five seconds";
        const int expected{ scenario == "exit" ? 42 : scenario == "trace" ? 1 : 128 + SIGKILL };
        EXPECT_EQ(session.childStatus(), expected);
        EXPECT_FALSE(session.snapshot().enabled);
        EXPECT_FALSE(session.snapshot().connected);
    }

    INSTANTIATE_TEST_SUITE_P(ChildTermination,
                             SimulationSession,
                             testing::Values("exit", "stall", "trace"),
                             [](const auto& info) { return info.param; });
}

int main(int argc, char** argv)
{
    // This ELF doubles as the child fixture; GoogleTest flags must never enter child mode.
    if (argc == 3 && std::string_view{ argv[1] } == "--session-child") {
        const char* fd{ std::getenv("PROTOMATE_SIM_FD") };
        if (!fd)
            return 2;
        simulation::Mapping mapping{ std::atoi(fd) };
        simulation::release(mapping.memory().closed, 1U);
        if (std::string_view{ argv[2] } == "stall")
            raise(SIGSTOP);
        usleep(200000);
        return std::string_view{ argv[2] } == "trace" ? 0 : 42;
    }
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
