#include "libc/filesystem.hpp"

#include <fx_api.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

namespace
{
    constexpr ULONG SECTORS{ 128U };
    constexpr ULONG SECTOR_BYTES{ 512U };
    struct Disk
    {
        std::array<UCHAR, SECTORS * SECTOR_BYTES> bytes{};
        std::array<UCHAR, SECTOR_BYTES> cache{};
        FX_MEDIA media{};
        bool opened{};
        ~Disk()
        {
            if (opened) {
                static_cast<void>(fx_media_close(&media));
            }
        }
    };

    void driver(FX_MEDIA* media)
    {
        auto& disk{ *static_cast<Disk*>(media->fx_media_driver_info) };
        media->fx_media_driver_status = FX_SUCCESS;
        switch (media->fx_media_driver_request) {
            case FX_DRIVER_READ:
            case FX_DRIVER_WRITE:
            case FX_DRIVER_BOOT_READ:
            case FX_DRIVER_BOOT_WRITE: {
                const bool boot{ media->fx_media_driver_request == FX_DRIVER_BOOT_READ ||
                                 media->fx_media_driver_request == FX_DRIVER_BOOT_WRITE };
                const ULONG sector{ boot ? 0U : media->fx_media_driver_logical_sector };
                const ULONG count{ boot ? 1U : media->fx_media_driver_sectors };
                if (sector > SECTORS || count > SECTORS - sector) {
                    media->fx_media_driver_status = FX_IO_ERROR;
                    return;
                }
                auto* address{ disk.bytes.data() + sector * SECTOR_BYTES };
                if (media->fx_media_driver_request == FX_DRIVER_READ ||
                    media->fx_media_driver_request == FX_DRIVER_BOOT_READ) {
                    std::memcpy(media->fx_media_driver_buffer, address, count * SECTOR_BYTES);
                }
                else {
                    std::memcpy(address, media->fx_media_driver_buffer, count * SECTOR_BYTES);
                }
                return;
            }
            default:
                return;
        }
    }
}

TEST(FileXDependency, DeletesPreviouslyPopulatedDirectoryOnSmallFatVolume)
{
    runtime_filex_initialize();
    const auto disk{ std::make_unique<Disk>() };
    ASSERT_EQ(fx_media_format(&disk->media,
                              driver,
                              disk.get(),
                              disk->cache.data(),
                              disk->cache.size(),
                              const_cast<CHAR*>("PATCH TEST"),
                              1U,
                              32U,
                              0U,
                              SECTORS,
                              SECTOR_BYTES,
                              1U,
                              1U,
                              1U),
              FX_SUCCESS);
    ASSERT_EQ(fx_media_open(&disk->media,
                            const_cast<CHAR*>("PATCH TEST"),
                            driver,
                            disk.get(),
                            disk->cache.data(),
                            disk->cache.size()),
              FX_SUCCESS);
    disk->opened = true;
    ASSERT_EQ(fx_directory_create(&disk->media, const_cast<CHAR*>("history")), FX_SUCCESS);
    for (unsigned i{}; i < 150U; ++i) {
        std::array<char, 32U> name{};
        std::snprintf(name.data(), name.size(), "history/%04u.txt", i);
        ASSERT_EQ(fx_file_create(&disk->media, name.data()), FX_SUCCESS);
    }
    for (unsigned i{}; i < 150U; ++i) {
        std::array<char, 32U> name{};
        std::snprintf(name.data(), name.size(), "history/%04u.txt", i);
        ASSERT_EQ(fx_file_delete(&disk->media, name.data()), FX_SUCCESS);
    }
    EXPECT_EQ(fx_directory_delete(&disk->media, const_cast<CHAR*>("history")), FX_SUCCESS);
}

TEST(FileXDependency, RejectsCorruptBootSectorWithoutFormattingIt)
{
    runtime_filex_initialize();
    const auto disk{ std::make_unique<Disk>() };
    disk->bytes.fill(0xA5U);
    const auto before{ std::make_unique<decltype(disk->bytes)>(disk->bytes) };
    EXPECT_NE(fx_media_open(&disk->media,
                            const_cast<CHAR*>("INVALID"),
                            driver,
                            disk.get(),
                            disk->cache.data(),
                            disk->cache.size()),
              FX_SUCCESS);
    EXPECT_EQ(disk->bytes, *before);
}
