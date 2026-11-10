#include "ControllerProcess.hpp"
#include "simulation/SharedMemory.hpp"
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace simulation
{
    namespace
    {
        constexpr int EXEC_FAILURE_STATUS{ 127 }, SIGNAL_EXIT_BASE{ 128 }, STARTUP_POLL_MS{ 10 };
        constexpr std::uint64_t STARTUP_TIMEOUT_NS{ 5'000'000'000ULL }, NS_PER_MS{ 1'000'000 };
        constexpr unsigned WAIT_SLICE_US{ 1000 };
        constexpr auto TERMINATE_GRACE{ std::chrono::milliseconds{ 100 } };
        struct ChildLaunch
        {
            int executable_fd;
            pid_t parent;
            std::span<char*> arguments, environment;
            std::span<const int> descriptors;
            std::array<int, 2> errors;
        };
        [[noreturn]] void execute_child(const ChildLaunch& launch) noexcept
        {
            close(launch.errors[0]);
            int code{};
            // Linux prctl has a fixed kernel ABI exposed through varargs.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
            if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0) {
                code = errno;
            }
            if (getppid() != launch.parent) {
                _exit(EXEC_FAILURE_STATUS);
            }
            for (int descriptor : launch.descriptors) {
                // POSIX/Linux variadic ABI with the argument types required by this operation.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
                if (!code && fcntl(descriptor, F_SETFD, 0) < 0) {
                    code = errno;
                }
            }
            if (!code) {
                fexecve(launch.executable_fd, launch.arguments.data(), launch.environment.data());
                code = errno;
            }
            // No allocation, C++ destruction or inherited runtime callbacks here.
            const auto data{ std::as_bytes(std::span{ &code, 1 }) };
            std::size_t sent{};
            while (sent < sizeof(code)) {
                const auto n{ write(launch.errors[1], data.data() + sent, sizeof(code) - sent) };
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                if (n <= 0) {
                    break;
                }
                sent += static_cast<std::size_t>(n);
            }
            _exit(EXEC_FAILURE_STATUS);
        }
    }

    namespace
    {
        struct Acknowledgement
        {
            int code{};
            std::size_t received{};
            ssize_t count{};
            int error{};
        };
        Acknowledgement read_acknowledgement(int fd)
        {
            int code{};
            std::size_t received{};
            ssize_t count{};
            const auto deadline{ host_now() + STARTUP_TIMEOUT_NS };
            for (;;) {
                pollfd ready{ .fd = fd, .events = POLLIN | POLLHUP, .revents = 0 };
                const int ready_count{ ::poll(&ready, 1, STARTUP_POLL_MS) };
                if (ready_count < 0 && errno != EINTR) {
                    count = -1;
                    break;
                }
                if (host_now() >= deadline) {
                    errno = ETIMEDOUT;
                    count = -1;
                    break;
                }
                if (ready_count <= 0) {
                    continue;
                }
                count = read(fd,
                             std::as_writable_bytes(std::span{ &code, 1 }).data() + received,
                             sizeof(code) - received);
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                if (count > 0) {
                    received += static_cast<std::size_t>(count);
                }
                if (count <= 0 || received == sizeof(code)) {
                    break;
                }
            }
            const int read_error{ errno };
            return { .code = code, .received = received, .count = count, .error = read_error };
        }
    }
    void ControllerProcess::launch(int executable_fd,
                                   std::span<char*> arguments,
                                   const std::array<int, DESCRIPTOR_COUNT>& descriptors)
    {
        if (m_launched) {
            throw std::logic_error{ "controller process is one-shot" };
        }
        constexpr std::array<const char*, DESCRIPTOR_COUNT> names{ "PROTOMATE_SIM_FD",
                                                                   "PROTOMATE_SIM_REQUEST_FD",
                                                                   "PROTOMATE_SIM_RESPONSE_FD",
                                                                   "PROTOMATE_SIM_INTERRUPT_FD",
                                                                   "PROTOMATE_SIM_ACK_FD" };
        std::array<std::string, DESCRIPTOR_COUNT> variables{};
        std::vector<char*> environment{};
        for (auto** entry{ environ }; *entry; ++entry) {
            if (!std::string_view{ *entry }.starts_with("PROTOMATE_SIM_")) {
                environment.push_back(*entry);
            }
        }
        for (unsigned i{}; i < names.size(); ++i) {
            variables[i] = std::string{ names[i] } + "=" + std::to_string(descriptors[i]);
            environment.push_back(variables[i].data());
        }
        environment.push_back(nullptr);
        auto name{ std::to_array("Application/controller") };
        std::vector<char*> argv{ name.data() };
        argv.insert(argv.end(), arguments.begin(), arguments.end());
        argv.push_back(nullptr);
        // EOF acknowledges exec, while an errno value reports setup/exec failure.
        std::array<int, 2> errors{};
        if (pipe2(errors.data(), O_CLOEXEC) < 0) {
            throw std::system_error{ errno, std::generic_category(), "controller startup pipe" };
        }
        const auto parent{ getpid() };
        const auto child{ fork() };
        if (child < 0) {
            const int code{ errno };
            close(errors[0]);
            close(errors[1]);
            throw std::system_error{ code, std::generic_category(), "fork controller" };
        }
        if (child == 0) {
            execute_child({ .executable_fd = executable_fd,
                            .parent = parent,
                            .arguments = argv,
                            .environment = environment,
                            .descriptors = descriptors,
                            .errors = errors });
        }
        m_child = child;
        m_launched = true;
        close(errors[1]);
        const auto acknowledgement{ read_acknowledgement(errors[0]) };
        const auto code{ acknowledgement.code };
        const auto received{ acknowledgement.received };
        const auto count{ acknowledgement.count };
        const auto read_error{ acknowledgement.error };
        close(errors[0]);
        if (count < 0 || received) {
            if (count < 0) {
                finish(std::chrono::milliseconds{ 0 });
                throw std::system_error{ read_error,
                                         std::generic_category(),
                                         "controller startup acknowledgement" };
            }
            // A child stopped after writing errno must not block launch forever.
            finish(TERMINATE_GRACE);
            throw std::system_error{ received == sizeof(code) ? code : EIO,
                                     std::generic_category(),
                                     "execute embedded controller" };
        }
    }

    ControllerProcess::~ControllerProcess() { finish(); }
    std::optional<int> ControllerProcess::poll() noexcept
    {
        if (m_child <= 0) {
            return m_result;
        }
        int status{};
        pid_t result{ waitpid(m_child, &status, WNOHANG) };
        while (result < 0 && errno == EINTR) {
            result = waitpid(m_child, &status, WNOHANG);
        }
        if (result == m_child) {
            m_result = WIFEXITED(status) ? WEXITSTATUS(status) : SIGNAL_EXIT_BASE + WTERMSIG(status);
            m_child = -1;
        }
        else if (result < 0) {
            // Do not signal a PID whose ownership can no longer be established.
            m_result = 1;
            m_child = -1;
        }
        return m_result;
    }
    int ControllerProcess::finish(std::chrono::milliseconds grace) noexcept
    {
        if (m_child <= 0) {
            return m_result.value_or(0);
        }
        const auto wait{ [&](std::chrono::milliseconds duration) {
            const auto end{ host_now() + (static_cast<std::uint64_t>(duration.count()) * NS_PER_MS) };
            while (!poll() && host_now() < end) {
                usleep(WAIT_SLICE_US);
            }
        } };
        wait(grace);
        if (m_child > 0) {
            kill(m_child, SIGTERM);
            wait(TERMINATE_GRACE);
        }
        if (m_child > 0) {
            kill(m_child, SIGKILL);
            int status{};
            pid_t result{ waitpid(m_child, &status, 0) };
            while (result < 0 && errno == EINTR) {
                result = waitpid(m_child, &status, 0);
            }
            m_result = result > 0 && WIFSIGNALED(status) ? SIGNAL_EXIT_BASE + WTERMSIG(status) : 1;
            m_child = -1;
        }
        return m_result.value_or(1);
    }
}
