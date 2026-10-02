#pragma once

#include <fx_api.h>

#include <cstdint>

namespace runtime::storage
{
    enum class Volume : std::uint8_t
    {
        flash,
        sd,
    };

    struct Diagnostics
    {
        std::uint32_t flash_jedec_id{};
        std::uint32_t flash_blocks{};
        std::uint32_t flash_logical_sectors{};
        std::uint32_t sd_blocks{};
        std::uint32_t qspi_error{};
        std::uint32_t sd_error{};
        std::uint32_t sd_fallback_error{};
        std::uint32_t levelx_status{};
        std::uint32_t flash_filex_status{};
        std::uint32_t sd_filex_status{};
        std::uint8_t flash_mounted{};
        std::uint8_t sd_mounted{};
        std::uint8_t flash_was_formatted{};
        std::uint8_t sd_detect_level{};
        std::uint8_t sd_bus_width{};
        std::uint8_t sd_fallback_used{};
    };

    [[nodiscard]] auto initialize() noexcept -> bool;
    [[nodiscard]] auto media(Volume volume) noexcept -> FX_MEDIA*;
    [[nodiscard]] auto mounted(Volume volume) noexcept -> bool;
    [[nodiscard]] auto diagnostics() noexcept -> const Diagnostics&;
}

extern "C" runtime::storage::Diagnostics runtime_storage_diagnostics;
