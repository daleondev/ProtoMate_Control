#pragma once

#include <fx_api.h>

#include <cstdint>

namespace runtime::storage::detail
{
    inline constexpr std::uint32_t sd_data_crc_error{ 0x0000'0002U };

    [[nodiscard]] constexpr auto valid_sector_range(std::uint64_t sector,
                                                    std::uint64_t hidden_sectors,
                                                    std::uint64_t count,
                                                    std::uint64_t capacity) noexcept -> bool
    {
        // Check each subtraction before adding a partition offset or sector
        // count: either addition could wrap into another part of the media.
        return hidden_sectors <= capacity && sector <= capacity - hidden_sectors &&
               count <= capacity - hidden_sectors - sector;
    }

    [[nodiscard]] constexpr auto should_retry_sd_mount_in_one_bit(std::uint8_t bus_width,
                                                                  UINT filex_status,
                                                                  std::uint32_t sd_error) noexcept -> bool
    {
        return bus_width == 4U && filex_status == FX_BOOT_ERROR && (sd_error & sd_data_crc_error) != 0U;
    }
}
