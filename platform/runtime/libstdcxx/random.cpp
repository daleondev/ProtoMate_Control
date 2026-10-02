#include "hal/drivers/factory/rng.hpp"

#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace
{
    [[nodiscard]] auto hardware_rng() -> hal::IRng&
    {
        static const auto rng{ hal::rng::create() };
        if (rng == nullptr) {
            throw std::runtime_error{ "hardware random-number generator is unavailable" };
        }
        return *rng;
    }

    auto validate_token(std::string_view token) -> void
    {
        if (token != "default" && token != "hardware") {
            throw std::runtime_error{ "unsupported random_device token: " + std::string{ token } };
        }
    }

    [[nodiscard]] auto hardware_random_value() -> std::random_device::result_type
    {
        const auto value{ hardware_rng().generate() };
        if (!value) {
            throw std::system_error{ value.error(), "hardware random-number generation failed" };
        }
        return *value;
    }
}

namespace std
{
    // The bare-metal libstdc++ supplied by the ARM toolchain implements
    // random_device with a deterministic mt19937 fallback. These definitions
    // replace that archive member and make the HAL entropy source mandatory.
    void random_device::_M_init(const string& token) { validate_token(token); }

    void random_device::_M_init_pretr1(const string& token) { _M_init(token); }

    void random_device::_M_init(const char* token, size_t length)
    {
        if (token == nullptr) {
            throw runtime_error{ "random_device token is null" };
        }
        validate_token(string_view{ token, length });
    }

    void random_device::_M_fini() { }

    random_device::result_type random_device::_M_getval() { return hardware_random_value(); }

    random_device::result_type random_device::_M_getval_pretr1() { return hardware_random_value(); }

    double random_device::_M_getentropy() const noexcept
    {
        return static_cast<double>(numeric_limits<result_type>::digits);
    }
}
