#include "Rng.hpp"

#include <system_error>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <sys/random.h>

namespace hal
{
    auto Rng::generate() noexcept -> util::Result<Value>
    {
        Value value{};
        auto* output{ reinterpret_cast<std::byte*>(&value) };
        std::size_t generated{};

        while (generated < sizeof(value)) {
            const ssize_t result{ getrandom(output + generated, sizeof(value) - generated, 0) };
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::unexpected(std::error_code{ errno, std::generic_category() });
            }
            if (result == 0) {
                return std::unexpected(std::make_error_code(std::errc::io_error));
            }
            generated += static_cast<std::size_t>(result);
        }
        return value;
    }
}
