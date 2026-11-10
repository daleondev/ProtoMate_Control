#include "ExecutableImage.hpp"
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>

namespace simulation
{
    ExecutableImage::ExecutableImage(std::span<const std::byte> bytes)
    {
        if (bytes.empty()) {
            throw std::invalid_argument{ "empty controller image" };
        }
        unsigned flags{ MFD_CLOEXEC | MFD_ALLOW_SEALING };
#ifdef MFD_EXEC
        flags |= MFD_EXEC;
#endif
        m_fd = memfd_create("Application-controller", flags);
        if (m_fd < 0) {
            throw std::system_error{ errno, std::generic_category(), "create controller image" };
        }
        try {
            std::size_t offset{};
            while (offset < bytes.size()) {
                const auto count{ write(m_fd, bytes.data() + offset, bytes.size() - offset) };
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                if (count <= 0) {
                    throw std::system_error{ count < 0 ? errno : EIO,
                                             std::generic_category(),
                                             "write controller image" };
                }
                offset += static_cast<std::size_t>(count);
            }
            // Linux file sealing is exposed through the variadic fcntl ABI.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
            if (fcntl(m_fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0) {
                throw std::system_error{ errno, std::generic_category(), "seal controller image" };
            }
        } catch (...) {
            close(m_fd);
            m_fd = -1;
            throw;
        }
    }
    ExecutableImage::~ExecutableImage()
    {
        if (m_fd >= 0) {
            close(m_fd);
        }
    }
}
