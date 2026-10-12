#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace runtime::filex
{
    constexpr std::size_t MAXIMUM_PATH{ 256U };
    constexpr std::size_t MAXIMUM_OPEN_FILES{ 16U };
    constexpr std::size_t MAXIMUM_OPEN_DIRECTORIES{ 8U };

    struct EntryInformation
    {
        std::uint64_t size{};
        std::uint32_t attributes{};
        std::uint16_t year{};
        std::uint8_t month{};
        std::uint8_t day{};
        std::uint8_t hour{};
        std::uint8_t minute{};
        std::uint8_t second{};
    };

    [[nodiscard]] auto initialized() noexcept -> bool;
    [[nodiscard]] auto normalize_path(const char* path, std::span<char, MAXIMUM_PATH> output) noexcept -> int;
    [[nodiscard]] auto information(const char* path, EntryInformation& result) noexcept -> int;
    [[nodiscard]] auto available_space(std::uint64_t& bytes) noexcept -> int;
    [[nodiscard]] auto available_space(const char* path, std::uint64_t& bytes) noexcept -> int;
    [[nodiscard]] auto current_path(std::span<char, MAXIMUM_PATH> output) noexcept -> int;
    [[nodiscard]] auto set_current_path(const char* path) noexcept -> int;
}

extern "C" void runtime_filex_initialize();
