#include "Rng.hpp"

#include "hal/drivers/common.hpp"

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
                return make_error_result<Value>(HalError::Error);
            }
            if (result == 0) {
                return make_error_result<Value>(HalError::Error);
            }
            generated += static_cast<std::size_t>(result);
        }
        return value;
    }
}
