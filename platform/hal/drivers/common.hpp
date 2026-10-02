#pragma once

#include "hal/hal.hpp"
#include "hal/utilities/Result.hpp"

#include <cstdint>
#include <system_error>
#include <utility>

namespace hal
{
    enum class HalError : std::uint8_t
    {
        Error = 1U,
        Busy = 2U,
        Timeout = 3U
    };

    class HalErrorCategory final : public std::error_category
    {
      public:
        const char* name() const noexcept override { return "HalError"; }

        std::string message(int ev) const override
        {
            switch (static_cast<HalError>(ev)) {
                using enum HalError;
                case Error:
                    return "Error";
                case Busy:
                    return "Busy";
                case Timeout:
                    return "Timeout";
                default:
                    return "Unknown";
            }
        }
    };

    [[nodiscard]] inline auto hal_error_category() noexcept -> const std::error_category&
    {
        static HalErrorCategory instance;
        return instance;
    }

    [[nodiscard]] inline auto make_error_code(HalError err) noexcept -> std::error_code
    {
        return { static_cast<int>(err), hal_error_category() };
    }

    [[nodiscard]] inline auto make_result(HAL_StatusTypeDef status) noexcept -> util::Result<>
    {
        if (status != HAL_OK) {
            return std::unexpected(make_error_code(static_cast<HalError>(status)));
        }
        return {};
    }

    template<typename T = void>
    [[nodiscard]] inline auto make_error_result(HalError error) noexcept -> util::Result<T>
    {
        return std::unexpected(make_error_code(error));
    }
}
