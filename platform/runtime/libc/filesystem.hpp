#pragma once

#include <cstddef>
#include <cstdint>

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
    [[nodiscard]] auto normalizePath(const char* path, char (&output)[MAXIMUM_PATH]) noexcept -> int;
    [[nodiscard]] auto information(const char* path, EntryInformation& result) noexcept -> int;
    [[nodiscard]] auto availableSpace(std::uint64_t& bytes) noexcept -> int;
    [[nodiscard]] auto availableSpace(const char* path, std::uint64_t& bytes) noexcept -> int;
    [[nodiscard]] auto currentPath(char (&output)[MAXIMUM_PATH]) noexcept -> int;
    [[nodiscard]] auto setCurrentPath(const char* path) noexcept -> int;
}

extern "C" void runtime_filex_initialize();
