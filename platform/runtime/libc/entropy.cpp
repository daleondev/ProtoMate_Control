#include "hal/core/core.hpp"
#include "hal/hal.hpp"
#include <tx_api.h>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>

// Required Newlib/BSP ABI spelling.
// NOLINTNEXTLINE(bugprone-reserved-identifier,readability-identifier-naming)
extern "C" int _getentropy(void* buffer, std::size_t length)
{
    constexpr std::size_t maximum_entropy_request{ 256U };
    if (length == 0U) {
        return 0;
    }
    if (buffer == nullptr) {
        errno = EFAULT;
        return -1;
    }
    if (length > maximum_entropy_request) {
        errno = EIO;
        return -1;
    }
    if (__get_IPSR() != 0U || tx_thread_identify() == TX_NULL) {
        errno = EPERM;
        return -1;
    }
    const auto rng{ hal::core::create_rng() };
    if (!rng) {
        errno = ENODEV;
        return -1;
    }
    auto* output{ static_cast<std::uint8_t*>(buffer) };
    std::size_t generated{};
    while (generated < length) {
        const auto random_value{ rng->generate() };
        if (!random_value) {
            errno = EIO;
            return -1;
        }
        for (std::size_t index{}; index < sizeof(*random_value) && generated < length; ++index) {
            output[generated++] = static_cast<std::uint8_t>(*random_value >> (index * CHAR_BIT));
        }
    }
    return 0;
}
