#pragma once

#include "Parser.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cli::filesystem
{
    using PathResult = std::expected<std::filesystem::path, CallbackError>;
    using WildcardResult = std::expected<std::vector<std::string>, CallbackError>;

    /**
     * Resolve a path against the CLI-local working directory without changing
     * the process working directory.
     */
    [[nodiscard]] auto resolvePath(std::string_view value,
                                   std::string_view argument_name = "path") -> PathResult;

    /**
     * Expand active '*' characters against the CLI-local working directory.
     * Mask entries are nonzero only for unquoted and unescaped wildcards.
     */
    [[nodiscard]] auto expandWildcards(std::string_view pattern,
                                       std::span<const std::uint8_t> wildcard_mask)
      -> WildcardResult;

    auto setup() -> void;
}
