#include "../ExecutableImage.hpp"
#include "../Session.hpp"
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>

namespace
{
    constexpr std::array INVALID_IMAGE{ std::byte{ 'n' },
                                        std::byte{ 'o' },
                                        std::byte{ 'p' },
                                        std::byte{ 'e' } };
}

TEST(SimulationLaunch, RejectsEmptyExecutable)
{
    EXPECT_THROW((simulation::ExecutableImage{ {} }), std::invalid_argument);
}

TEST(SimulationLaunch, ExecutableImageIsSealed)
{
    simulation::ExecutableImage image{ INVALID_IMAGE };
    constexpr int required{ F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL };
    const int seals{ fcntl(image.fd(), F_GET_SEALS) };
    ASSERT_GE(seals, 0);
    EXPECT_EQ(seals & required, required);
    EXPECT_EQ(pwrite(image.fd(), INVALID_IMAGE.data(), INVALID_IMAGE.size(), 0), -1);
    EXPECT_EQ(errno, EPERM);
    EXPECT_EQ(ftruncate(image.fd(), 0), -1);
    EXPECT_EQ(errno, EPERM);
}

TEST(SimulationLaunch, FailedExecReapsChildAndCannotStart)
{
    simulation::ExecutableImage image{ INVALID_IMAGE };
    simulation::Session session;
    try {
        session.launch(image.fd(), {});
        FAIL() << "Invalid ELF accepted";
    } catch (const std::system_error& error) {
        EXPECT_EQ(error.code().value(), ENOEXEC);
    }
    int status{};
    EXPECT_EQ(waitpid(-1, &status, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD);
    EXPECT_FALSE(session.running());
    EXPECT_THROW(session.start(), std::logic_error);
}
