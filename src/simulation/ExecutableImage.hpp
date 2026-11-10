#pragma once
#include <cstddef>
#include <span>

namespace simulation
{
    // Owns the sealed, anonymous executable backing for the controller child.
    class ExecutableImage final
    {
      public:
        ExecutableImage(ExecutableImage&&) = delete;
        auto operator=(ExecutableImage&&) -> ExecutableImage& = delete;

        explicit ExecutableImage(std::span<const std::byte> bytes);
        ~ExecutableImage();
        ExecutableImage(const ExecutableImage&) = delete;
        ExecutableImage& operator=(const ExecutableImage&) = delete;
        int fd() const noexcept { return m_fd; }

      private:
        int m_fd{ -1 };
    };
}
