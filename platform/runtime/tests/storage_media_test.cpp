#include "libc/storage_media_policy.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace
{
    using runtime::storage::detail::sd_data_crc_error;
    using runtime::storage::detail::should_retry_sd_mount_in_one_bit;
    using runtime::storage::detail::valid_sector_range;
}

TEST(StorageMediaPolicy, ValidatesPartitionOffsetsAndCountsWithoutOverflow)
{
    EXPECT_TRUE(valid_sector_range(0U, 0U, 1U, 100U));
    EXPECT_TRUE(valid_sector_range(9U, 90U, 1U, 100U));
    EXPECT_TRUE(valid_sector_range(10U, 90U, 0U, 100U));
    EXPECT_FALSE(valid_sector_range(10U, 90U, 1U, 100U));
    EXPECT_FALSE(valid_sector_range(0U, 101U, 0U, 100U));
    EXPECT_FALSE(valid_sector_range(101U, 0U, 0U, 100U));
    constexpr auto maximum{ std::numeric_limits<std::uint64_t>::max() };
    EXPECT_FALSE(valid_sector_range(maximum, 1U, 0U, maximum));
    EXPECT_FALSE(valid_sector_range(1U, 0U, maximum, maximum));
    EXPECT_FALSE(valid_sector_range(0xFFFF'FFFFU, 2U, 1U, 100U));
}

TEST(StorageMediaPolicy, RetriesOnlyFourBitBootReadsThatFailCrcValidation)
{
    EXPECT_TRUE(should_retry_sd_mount_in_one_bit(4U, FX_BOOT_ERROR, sd_data_crc_error));
    EXPECT_TRUE(should_retry_sd_mount_in_one_bit(4U, FX_BOOT_ERROR, sd_data_crc_error | 0x80U));

    EXPECT_FALSE(should_retry_sd_mount_in_one_bit(1U, FX_BOOT_ERROR, sd_data_crc_error));
    EXPECT_FALSE(should_retry_sd_mount_in_one_bit(4U, FX_SUCCESS, sd_data_crc_error));
    EXPECT_FALSE(should_retry_sd_mount_in_one_bit(4U, FX_MEDIA_INVALID, sd_data_crc_error));
    EXPECT_FALSE(should_retry_sd_mount_in_one_bit(4U, FX_BOOT_ERROR, 0U));
}
