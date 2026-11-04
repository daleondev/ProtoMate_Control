#include "../ExecutableImage.hpp"
#include "../Session.hpp"
#include <array>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>

namespace
{
    void require(bool value, const char* message)
    {
        if (!value)
            throw std::runtime_error{ message };
    }
}
int main()
try {
    bool empty_rejected{};
    try {
        simulation::ExecutableImage empty{ {} };
    } catch (const std::invalid_argument&) {
        empty_rejected = true;
    }
    require(empty_rejected, "empty controller image accepted");
    const std::array<std::byte, 4> invalid{
        std::byte{ 'n' }, std::byte{ 'o' }, std::byte{ 'p' }, std::byte{ 'e' }
    };
    simulation::ExecutableImage image{ invalid };
    const auto required{ F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL };
    require((fcntl(image.fd(), F_GET_SEALS) & required) == required, "controller image is not fully sealed");
    require(pwrite(image.fd(), invalid.data(), invalid.size(), 0) < 0 && errno == EPERM,
            "sealed image can be modified");
    require(ftruncate(image.fd(), 0) < 0 && errno == EPERM, "sealed image can be resized");
    simulation::Session session{};
    bool exec_rejected{};
    try {
        session.launch(image.fd(), {});
    } catch (const std::system_error& error) {
        exec_rejected = error.code().value() == ENOEXEC;
    }
    require(exec_rejected, "invalid ELF did not report exec failure");
    int status{};
    require(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "failed child not reaped");
    require(!session.running(), "failed launch started simulation");
    bool start_rejected{};
    try {
        session.start();
    } catch (const std::logic_error&) {
        start_rejected = true;
    }
    require(start_rejected, "worker started without a controller");
    std::puts("PASS: sealed executable image and synchronous exec failure with child reaping");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
