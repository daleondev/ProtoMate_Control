#include "../Session.hpp"
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <signal.h>
#include <string_view>
#include <unistd.h>

int main(int argc, char** argv)
try {
    if (argc == 2) {
        simulation::Mapping mapping{ std::atoi(std::getenv("PROTOMATE_SIM_FD")) };
        simulation::release(mapping.memory().closed, 1U);
        if (std::string_view{ argv[1] } == "stall")
            raise(SIGSTOP);
        usleep(200000);
        return std::string_view{ argv[1] } == "trace" ? 0 : 42;
    }
    for (const char* scenario : { "exit", "stall", "trace" }) {
        const int fd{ open(argv[0], O_RDONLY | O_CLOEXEC) };
        if (fd < 0)
            return 2;
        simulation::Session session{ { .trace_path =
                                         std::string_view{ scenario } == "trace" ? "/dev/full" : "" } };
        auto* argument{ const_cast<char*>(scenario) };
        session.launch(fd, { &argument, 1 });
        close(fd);
        session.start();
        while (session.running())
            std::this_thread::sleep_for(std::chrono::milliseconds{ 10 });
        const int expected{ std::string_view{ scenario } == "exit"    ? 42
                            : std::string_view{ scenario } == "trace" ? 1
                                                                      : 128 + SIGKILL };
        if (session.childStatus() != expected)
            throw std::runtime_error{ "closing IPC hid the final child exit status" };
        if (session.snapshot().enabled || session.snapshot().connected)
            throw std::runtime_error{ "final snapshot still reports an active session" };
    }
    std::puts("PASS: final child status and bounded reaping after early IPC close");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
