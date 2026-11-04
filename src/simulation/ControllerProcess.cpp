#include "ControllerProcess.hpp"
#include "simulation/SharedMemory.hpp"
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <vector>
extern char** environ;

namespace simulation
{
    void ControllerProcess::launch(int executable_fd,
                                   std::span<char*> arguments,
                                   const std::array<int, 5>& descriptors)
    {
        if (m_launched)
            throw std::logic_error{ "controller process is one-shot" };
        constexpr std::array<const char*, 5> names{ "PROTOMATE_SIM_FD",
                                                    "PROTOMATE_SIM_REQUEST_FD",
                                                    "PROTOMATE_SIM_RESPONSE_FD",
                                                    "PROTOMATE_SIM_INTERRUPT_FD",
                                                    "PROTOMATE_SIM_ACK_FD" };
        std::array<std::string, 5> variables{};
        std::vector<char*> environment{};
        for (auto** entry{ environ }; *entry; ++entry)
            if (!std::string_view{ *entry }.starts_with("PROTOMATE_SIM_"))
                environment.push_back(*entry);
        for (unsigned i{}; i < names.size(); ++i) {
            variables[i] = std::string{ names[i] } + "=" + std::to_string(descriptors[i]);
            environment.push_back(variables[i].data());
        }
        environment.push_back(nullptr);
        std::array<char, 23> name{ "Application/controller" };
        std::vector<char*> argv{ name.data() };
        argv.insert(argv.end(), arguments.begin(), arguments.end());
        argv.push_back(nullptr);
        // EOF acknowledges exec, while an errno value reports setup/exec failure.
        std::array<int, 2> errors{};
        if (pipe2(errors.data(), O_CLOEXEC) < 0)
            throw std::system_error{ errno, std::generic_category(), "controller startup pipe" };
        const auto parent{ getpid() };
        const auto child{ fork() };
        if (child < 0) {
            const int code{ errno };
            close(errors[0]);
            close(errors[1]);
            throw std::system_error{ code, std::generic_category(), "fork controller" };
        }
        if (child == 0) {
            close(errors[0]);
            int code{};
            if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0)
                code = errno;
            if (getppid() != parent)
                _exit(127);
            for (int descriptor : descriptors)
                if (!code && fcntl(descriptor, F_SETFD, 0) < 0)
                    code = errno;
            if (!code) {
                fexecve(executable_fd, argv.data(), environment.data());
                code = errno;
            }
            // No allocation, C++ destruction or inherited runtime callbacks here.
            const auto* data{ reinterpret_cast<const char*>(&code) };
            std::size_t sent{};
            while (sent < sizeof(code)) {
                const auto n{ write(errors[1], data + sent, sizeof(code) - sent) };
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    break;
                sent += static_cast<std::size_t>(n);
            }
            _exit(127);
        }
        m_child = child;
        m_launched = true;
        close(errors[1]);
        int code{};
        std::size_t received{};
        ssize_t count{};
        const auto deadline{ hostNow() + 5'000'000'000ULL };
        for (;;) {
            pollfd ready{ errors[0], POLLIN | POLLHUP, 0 };
            const int ready_count{ ::poll(&ready, 1, 10) };
            if (ready_count < 0 && errno != EINTR) {
                count = -1;
                break;
            }
            if (hostNow() >= deadline) {
                errno = ETIMEDOUT;
                count = -1;
                break;
            }
            if (ready_count <= 0)
                continue;
            count = read(errors[0], reinterpret_cast<char*>(&code) + received, sizeof(code) - received);
            if (count < 0 && errno == EINTR)
                continue;
            if (count > 0)
                received += static_cast<std::size_t>(count);
            if (count <= 0 || received == sizeof(code))
                break;
        }
        const int read_error{ errno };
        close(errors[0]);
        if (count < 0 || received) {
            if (count < 0) {
                finish(std::chrono::milliseconds{ 0 });
                throw std::system_error{ read_error,
                                         std::generic_category(),
                                         "controller startup acknowledgement" };
            }
            // A child stopped after writing errno must not block launch forever.
            finish(std::chrono::milliseconds{ 100 });
            throw std::system_error{ received == sizeof(code) ? code : EIO,
                                     std::generic_category(),
                                     "execute embedded controller" };
        }
    }

    ControllerProcess::~ControllerProcess() { finish(); }
    std::optional<int> ControllerProcess::poll() noexcept
    {
        if (m_child <= 0)
            return m_result;
        int status{};
        pid_t result{};
        do {
            result = waitpid(m_child, &status, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == m_child) {
            m_result = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
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
        if (m_child <= 0)
            return m_result.value_or(0);
        const auto wait{ [&](std::chrono::milliseconds duration) {
            const auto end{ hostNow() + static_cast<std::uint64_t>(duration.count()) * 1'000'000 };
            while (!poll() && hostNow() < end)
                usleep(1000);
        } };
        wait(grace);
        if (m_child > 0) {
            kill(m_child, SIGTERM);
            wait(std::chrono::milliseconds{ 100 });
        }
        if (m_child > 0) {
            kill(m_child, SIGKILL);
            int status{};
            pid_t result{};
            do {
                result = waitpid(m_child, &status, 0);
            } while (result < 0 && errno == EINTR);
            m_result = result > 0 && WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
            m_child = -1;
        }
        return m_result.value_or(1);
    }
}
