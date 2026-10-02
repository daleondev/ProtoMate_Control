#pragma once

#include <expected>
#include <system_error>
#include <utility>

namespace util
{
    template<typename T = void>
    using Result = std::expected<T, std::error_code>;

    namespace result
    {
        constexpr auto success() noexcept -> Result<>
        {
            return {};
        }

        template<typename T>
        constexpr auto success(T value) -> Result<T>
        {
            return std::move(value);
        }

        template<typename T = void>
        constexpr auto fail(std::error_code error) noexcept -> Result<T>
        {
            return std::unexpected(error);
        }
    }
}
