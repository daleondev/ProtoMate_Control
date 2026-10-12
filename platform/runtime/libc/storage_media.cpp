#include "storage_media.hpp"
#include "storage_media_policy.hpp"

#include <lx_api.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <ranges>

#include <unistd.h>

// FileX exports this ABI symbol.
// NOLINTNEXTLINE(bugprone-reserved-identifier,readability-identifier-naming)
extern "C" UINT _fx_partition_offset_calculate(void* partition_sector,
                                               UINT partition,
                                               ULONG* partition_start,
                                               ULONG* partition_size);


namespace
{
    constexpr ULONG SECTOR_SIZE{ 512U };
    constexpr ULONG FLASH_BYTES{ 16U * 1024U * 1024U };
    constexpr ULONG ERASE_BLOCK_BYTES{ 4U * 1024U };
    constexpr ULONG FLASH_BLOCKS{ FLASH_BYTES / ERASE_BLOCK_BYTES };
    constexpr ULONG FLASH_FILEX_SECTORS{ 24U * 1024U };
    constexpr std::size_t MEDIA_CACHE_BYTES{ std::size_t{ 4U } * 1024U };
    constexpr std::size_t LEVELX_CACHE_BYTES{ std::size_t{ 8U } * 1024U };
    constexpr std::uintptr_t LEVELX_BASE_ADDRESS{ 0x9000'0000U };

    constexpr std::size_t CACHE_LINE_BYTES{ 32U };
    constexpr std::size_t FLASH_PAGE_BYTES{ 256U };
    constexpr UCHAR ERASED_FLASH_BYTE{ 0xFFU };
    constexpr std::uint32_t FLASH_JEDEC_ID{ 0xEF4018U };
    constexpr UINT ROOT_DIRECTORY_ENTRIES{ 128U };

    struct FileCloser
    {
        // Invoked by unique_ptr, which transfers ownership to its deleter.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        void operator()(std::FILE* file) const noexcept { static_cast<void>(std::fclose(file)); }
    };
    using File = std::unique_ptr<std::FILE, FileCloser>;

    struct DirectMedia
    {
        File file;
        ULONG sectors{};
    };

    // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
    alignas(CACHE_LINE_BYTES) std::array<UCHAR, MEDIA_CACHE_BYTES> flash_media_cache{};
    alignas(CACHE_LINE_BYTES) std::array<UCHAR, MEDIA_CACHE_BYTES> sd_media_cache{};
    alignas(CACHE_LINE_BYTES) std::array<UCHAR, LEVELX_CACHE_BYTES> levelx_cache{};
    alignas(CACHE_LINE_BYTES) std::array<ULONG, SECTOR_SIZE / sizeof(ULONG)> levelx_sector_buffer{};
    // Each FileX media has its own mutex, so the two physical devices must
    // also have independent scratch storage when accessed concurrently.
    alignas(CACHE_LINE_BYTES) std::array<UCHAR, SECTOR_SIZE> flash_verify_buffer{};
    FX_MEDIA flash_media{};
    FX_MEDIA sd_media{};
    LX_NOR_FLASH nor_flash{};
    DirectMedia sd_device{};
    File nor_file{};
    bool nor_open{};
    bool initialization_attempted{};
    bool flash_mounted{};
    bool sd_mounted{};
    // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

    [[nodiscard]] auto byte_offset(const ULONG* address) noexcept -> std::size_t
    {
        // LevelX uses a pointer-shaped address token; the driver never dereferences it.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        return static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(address) - LEVELX_BASE_ADDRESS);
    }

    [[nodiscard]] auto file_read(std::FILE* file,
                                 std::size_t offset,
                                 void* destination,
                                 std::size_t bytes) noexcept -> bool
    {
        return file != nullptr && std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0 &&
               std::fread(destination, 1U, bytes, file) == bytes;
    }

    [[nodiscard]] auto file_write(std::FILE* file,
                                  std::size_t offset,
                                  const void* source,
                                  std::size_t bytes) noexcept -> bool
    {
        return file != nullptr && std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0 &&
               std::fwrite(source, 1U, bytes, file) == bytes && std::fflush(file) == 0;
    }

    struct ImageConfiguration
    {
        const char* kind;
        std::size_t bytes;
        UCHAR fill;
    };

    [[nodiscard]] auto open_image(const ImageConfiguration& configuration, File& file, bool& created) noexcept
      -> bool
    {
        constexpr std::size_t image_path_capacity{ 128U };
        std::array<char, image_path_capacity> path{};
        // Bounded formatting for a C filesystem path, without throwing allocations.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        const int length{ std::snprintf(path.data(),
                                        path.size(),
                                        "/tmp/protomate-%ld-%s.img",
                                        static_cast<long>(getpid()),
                                        configuration.kind) };
        if (length <= 0 || static_cast<std::size_t>(length) >= path.size()) {
            return false;
        }
        File opened{ std::fopen(path.data(), "r+b") };
        created = opened == nullptr;
        if (created) {
            File created_file{ std::fopen(path.data(), "w+b") };
            opened = std::move(created_file);
        }
        if (opened == nullptr) {
            return false;
        }
        if (created) {
            std::array<UCHAR, ERASE_BLOCK_BYTES> block{};
            block.fill(configuration.fill);
            for (std::size_t offset{}; offset < configuration.bytes; offset += block.size()) {
                const std::size_t count{ std::min(block.size(), configuration.bytes - offset) };
                if (std::fwrite(block.data(), 1U, count, opened.get()) != count) {
                    return false;
                }
            }
            if (std::fflush(opened.get()) != 0) {
                return false;
            }
        }
        file = std::move(opened);
        return true;
    }

    [[nodiscard]] auto qspi_read(std::size_t offset, void* destination, std::size_t bytes) noexcept -> bool
    {
        return offset <= FLASH_BYTES && bytes <= FLASH_BYTES - offset &&
               file_read(nor_file.get(), offset, destination, bytes);
    }

    [[nodiscard]] auto qspi_write(std::size_t offset, const void* source, std::size_t bytes) noexcept -> bool
    {
        if (offset > FLASH_BYTES || bytes > FLASH_BYTES - offset) {
            return false;
        }
        std::array<UCHAR, FLASH_PAGE_BYTES> current{};
        const auto* input{ static_cast<const UCHAR*>(source) };
        while (bytes != 0U) {
            const std::size_t count{ std::min(bytes, current.size()) };
            if (!file_read(nor_file.get(), offset, current.data(), count)) {
                return false;
            }
            for (std::size_t index{}; index < count; ++index) {
                if ((current[index] & input[index]) != input[index]) {
                    return false;
                }
            }
            if (!file_write(nor_file.get(), offset, input, count)) {
                return false;
            }
            offset += count;
            input += count;
            bytes -= count;
        }
        return true;
    }

    [[nodiscard]] auto qspi_erase(std::size_t offset) noexcept -> bool
    {
        std::array<UCHAR, ERASE_BLOCK_BYTES> erased{};
        erased.fill(ERASED_FLASH_BYTE);
        return offset < FLASH_BYTES && (offset % ERASE_BLOCK_BYTES) == 0U &&
               file_write(nor_file.get(), offset, erased.data(), erased.size());
    }

    [[nodiscard]] auto sd_read(ULONG sector, void* destination, UINT sectors) noexcept -> bool
    {
        return runtime::storage::detail::valid_sector_range(sector, 0U, sectors, sd_device.sectors) &&
               file_read(sd_device.file.get(),
                         static_cast<std::size_t>(sector) * SECTOR_SIZE,
                         destination,
                         static_cast<std::size_t>(sectors) * SECTOR_SIZE);
    }

    [[nodiscard]] auto sd_write(ULONG sector, const void* source, UINT sectors) noexcept -> bool
    {
        return runtime::storage::detail::valid_sector_range(sector, 0U, sectors, sd_device.sectors) &&
               file_write(sd_device.file.get(),
                          static_cast<std::size_t>(sector) * SECTOR_SIZE,
                          source,
                          static_cast<std::size_t>(sectors) * SECTOR_SIZE);
    }

    auto direct_media_driver(FX_MEDIA* media_ptr) noexcept -> void
    {
        media_ptr->fx_media_driver_status = FX_IO_ERROR;
        switch (media_ptr->fx_media_driver_request) {
            case FX_DRIVER_INIT:
            case FX_DRIVER_UNINIT:
            case FX_DRIVER_FLUSH:
            case FX_DRIVER_ABORT:
                media_ptr->fx_media_driver_status = FX_SUCCESS;
                return;
            case FX_DRIVER_READ:
                if (runtime::storage::detail::valid_sector_range(media_ptr->fx_media_driver_logical_sector,
                                                                 media_ptr->fx_media_hidden_sectors,
                                                                 media_ptr->fx_media_driver_sectors,
                                                                 runtime_storage_diagnostics.sd_blocks) &&
                    sd_read(media_ptr->fx_media_driver_logical_sector + media_ptr->fx_media_hidden_sectors,
                            media_ptr->fx_media_driver_buffer,
                            media_ptr->fx_media_driver_sectors)) {
                    media_ptr->fx_media_driver_status = FX_SUCCESS;
                }
                return;
            case FX_DRIVER_WRITE:
                if (runtime::storage::detail::valid_sector_range(media_ptr->fx_media_driver_logical_sector,
                                                                 media_ptr->fx_media_hidden_sectors,
                                                                 media_ptr->fx_media_driver_sectors,
                                                                 runtime_storage_diagnostics.sd_blocks) &&
                    sd_write(media_ptr->fx_media_driver_logical_sector + media_ptr->fx_media_hidden_sectors,
                             media_ptr->fx_media_driver_buffer,
                             media_ptr->fx_media_driver_sectors)) {
                    media_ptr->fx_media_driver_status = FX_SUCCESS;
                }
                return;
            case FX_DRIVER_BOOT_READ: {
                if (!sd_read(0U, media_ptr->fx_media_driver_buffer, 1U)) {
                    return;
                }
                ULONG partition_start{};
                ULONG partition_size{};
                const UINT status{ _fx_partition_offset_calculate(
                  media_ptr->fx_media_driver_buffer, 0U, &partition_start, &partition_size) };
                if (status != FX_SUCCESS) {
                    return;
                }
                if (partition_start != 0U &&
                    !sd_read(partition_start, media_ptr->fx_media_driver_buffer, 1U)) {
                    return;
                }
                media_ptr->fx_media_driver_status = FX_SUCCESS;
                return;
            }
            case FX_DRIVER_BOOT_WRITE:
                if (sd_write(media_ptr->fx_media_hidden_sectors, media_ptr->fx_media_driver_buffer, 1U)) {
                    media_ptr->fx_media_driver_status = FX_SUCCESS;
                }
                return;
            default:
                return;
        }
    }

    [[nodiscard]] auto nor_driver_read(ULONG* flash_address, ULONG* destination, ULONG words) noexcept -> UINT
    {
        return qspi_read(
                 byte_offset(flash_address), destination, static_cast<std::size_t>(words) * sizeof(ULONG))
                 ? LX_SUCCESS
                 : LX_ERROR;
    }

    [[nodiscard]] auto nor_driver_write(ULONG* flash_address, ULONG* source, ULONG words) noexcept -> UINT
    {
        return qspi_write(byte_offset(flash_address), source, static_cast<std::size_t>(words) * sizeof(ULONG))
                 ? LX_SUCCESS
                 : LX_ERROR;
    }

    [[nodiscard]] auto nor_driver_erase(ULONG block, ULONG /*unused*/) noexcept -> UINT
    {
        return block < FLASH_BLOCKS && qspi_erase(static_cast<std::size_t>(block) * ERASE_BLOCK_BYTES)
                 ? LX_SUCCESS
                 : LX_ERROR;
    }

    [[nodiscard]] auto nor_driver_verify(ULONG block) noexcept -> UINT
    {
        if (block >= FLASH_BLOCKS) {
            return LX_ERROR;
        }
        for (std::size_t offset{}; offset < ERASE_BLOCK_BYTES; offset += flash_verify_buffer.size()) {
            if (!qspi_read((static_cast<std::size_t>(block) * ERASE_BLOCK_BYTES) + offset,
                           flash_verify_buffer.data(),
                           flash_verify_buffer.size()) ||
                !std::ranges::all_of(flash_verify_buffer,
                                     [](UCHAR byte) { return byte == ERASED_FLASH_BYTE; })) {
                return LX_ERROR;
            }
        }
        return LX_SUCCESS;
    }

    [[nodiscard]] auto nor_driver_error(UINT error) noexcept -> UINT
    {
        runtime_storage_diagnostics.levelx_status = error;
        return LX_SUCCESS;
    }

    [[nodiscard]] auto nor_driver_initialize(LX_NOR_FLASH* flash) noexcept -> UINT
    {
        flash->lx_nor_flash_total_blocks = FLASH_BLOCKS;
        flash->lx_nor_flash_words_per_block = ERASE_BLOCK_BYTES / sizeof(ULONG);
        // LevelX requires a pointer-shaped base token; physical access goes through the driver.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
        flash->lx_nor_flash_base_address = reinterpret_cast<ULONG*>(LEVELX_BASE_ADDRESS);
        flash->lx_nor_flash_driver_read = nor_driver_read;
        flash->lx_nor_flash_driver_write = nor_driver_write;
        flash->lx_nor_flash_driver_block_erase = nor_driver_erase;
        flash->lx_nor_flash_driver_block_erased_verify = nor_driver_verify;
        flash->lx_nor_flash_driver_system_error = nor_driver_error;
        flash->lx_nor_flash_sector_buffer = levelx_sector_buffer.data();
        return LX_SUCCESS;
    }

    [[nodiscard]] auto open_levelx() noexcept -> UINT
    {
        if (nor_open) {
            return LX_SUCCESS;
        }
        static auto name{ std::to_array("W25Q128") };
        UINT status{ lx_nor_flash_open(&nor_flash, name.data(), nor_driver_initialize) };
        if (status == LX_SUCCESS) {
            status = lx_nor_flash_extended_cache_enable(
              &nor_flash, levelx_cache.data(), static_cast<ULONG>(levelx_cache.size()));
            if (status != LX_SUCCESS) {
                static_cast<void>(lx_nor_flash_close(&nor_flash));
            }
        }
        nor_open = status == LX_SUCCESS;
        runtime_storage_diagnostics.levelx_status = status;
        return status;
    }

    auto close_levelx() noexcept -> UINT
    {
        if (!nor_open) {
            return LX_SUCCESS;
        }
        const UINT status{ lx_nor_flash_close(&nor_flash) };
        nor_open = false;
        return status;
    }

    auto transfer_levelx_sectors(FX_MEDIA* media) noexcept -> UINT
    {
        const auto request{ media->fx_media_driver_request };
        const bool boot{ request == FX_DRIVER_BOOT_READ || request == FX_DRIVER_BOOT_WRITE };
        const bool write{ request == FX_DRIVER_WRITE || request == FX_DRIVER_BOOT_WRITE };
        const ULONG sector{ boot ? 0U : media->fx_media_driver_logical_sector };
        const UINT sectors{ boot ? 1U : media->fx_media_driver_sectors };
        if (!runtime::storage::detail::valid_sector_range(sector, 0U, sectors, FLASH_FILEX_SECTORS)) {
            return FX_IO_ERROR;
        }
        auto* buffer{ media->fx_media_driver_buffer };
        for (UINT index{}; index < sectors; ++index) {
            const UINT status{ write ? lx_nor_flash_sector_write(&nor_flash, sector + index, buffer)
                                     : lx_nor_flash_sector_read(&nor_flash, sector + index, buffer) };
            if (status != LX_SUCCESS) {
                return FX_IO_ERROR;
            }
            buffer += SECTOR_SIZE;
        }
        return FX_SUCCESS;
    }

    auto release_levelx_sectors(FX_MEDIA* media) noexcept -> UINT
    {
        if (!runtime::storage::detail::valid_sector_range(media->fx_media_driver_logical_sector,
                                                          0U,
                                                          media->fx_media_driver_sectors,
                                                          FLASH_FILEX_SECTORS)) {
            return FX_IO_ERROR;
        }
        for (UINT index{}; index < media->fx_media_driver_sectors; ++index) {
            const UINT status{ lx_nor_flash_sector_release(&nor_flash,
                                                           media->fx_media_driver_logical_sector + index) };
            if (status != LX_SUCCESS && status != LX_SECTOR_NOT_FOUND) {
                return FX_IO_ERROR;
            }
        }
        return FX_SUCCESS;
    }

    auto levelx_media_driver(FX_MEDIA* media_ptr) noexcept -> void
    {
        media_ptr->fx_media_driver_status = FX_IO_ERROR;
        switch (media_ptr->fx_media_driver_request) {
            case FX_DRIVER_INIT:
                if (open_levelx() == LX_SUCCESS) {
                    media_ptr->fx_media_driver_free_sector_update = FX_TRUE;
                    media_ptr->fx_media_driver_status = FX_SUCCESS;
                }
                return;
            case FX_DRIVER_UNINIT:
                media_ptr->fx_media_driver_status = close_levelx() == LX_SUCCESS ? FX_SUCCESS : FX_IO_ERROR;
                return;
            case FX_DRIVER_READ:
            case FX_DRIVER_BOOT_READ:
            case FX_DRIVER_WRITE:
            case FX_DRIVER_BOOT_WRITE:
                media_ptr->fx_media_driver_status = transfer_levelx_sectors(media_ptr);
                return;
            case FX_DRIVER_RELEASE_SECTORS:
                media_ptr->fx_media_driver_status = release_levelx_sectors(media_ptr);
                return;
            case FX_DRIVER_FLUSH:
            case FX_DRIVER_ABORT:
                media_ptr->fx_media_driver_status = FX_SUCCESS;
                return;
            default:
                return;
        }
    }

    [[nodiscard]] auto initialize_flash_media() noexcept -> bool
    {
        static auto name{ std::to_array("FLASH") };
        bool created{};
        if (!open_image({ .kind = "flash-levelx", .bytes = FLASH_BYTES, .fill = ERASED_FLASH_BYTE },
                        nor_file,
                        created)) {
            return false;
        }
        runtime_storage_diagnostics.flash_jedec_id = FLASH_JEDEC_ID;
        runtime_storage_diagnostics.flash_blocks = FLASH_BLOCKS;
        lx_nor_flash_initialize();
        if (open_levelx() != LX_SUCCESS) {
            return false;
        }
        runtime_storage_diagnostics.flash_logical_sectors = FLASH_FILEX_SECTORS;

        // A LevelX read of an unmapped logical sector allocates it. Determine
        // whether the device is blank from the mapping count without probing
        // sector zero, so the mount check itself never mutates the flash.
        const bool empty{ nor_flash.lx_nor_flash_mapped_physical_sectors == 0U };
        if (close_levelx() != LX_SUCCESS) {
            return false;
        }
        if (empty) {
            const UINT format_status{ fx_media_format(&flash_media,
                                                      levelx_media_driver,
                                                      nullptr,
                                                      flash_media_cache.data(),
                                                      flash_media_cache.size(),
                                                      name.data(),
                                                      1U,
                                                      ROOT_DIRECTORY_ENTRIES,
                                                      0U,
                                                      FLASH_FILEX_SECTORS,
                                                      SECTOR_SIZE,
                                                      4U,
                                                      1U,
                                                      1U) };
            if (format_status != FX_SUCCESS) {
                runtime_storage_diagnostics.flash_filex_status = format_status;
                return false;
            }
            runtime_storage_diagnostics.flash_was_formatted = 1U;
        }
        const UINT open_status{ fx_media_open(&flash_media,
                                              name.data(),
                                              levelx_media_driver,
                                              nullptr,
                                              flash_media_cache.data(),
                                              flash_media_cache.size()) };
        runtime_storage_diagnostics.flash_filex_status = open_status;
        flash_mounted = open_status == FX_SUCCESS;
        runtime_storage_diagnostics.flash_mounted = flash_mounted;
        return flash_mounted;
    }

    [[nodiscard]] auto initialize_sd_media() noexcept -> bool
    {
        static auto name{ std::to_array("SD") };
        constexpr ULONG linux_sd_sectors{ 128U * 1024U };
        bool created{};
        if (!open_image({ .kind = "sd-filex",
                          .bytes = static_cast<std::size_t>(linux_sd_sectors) * SECTOR_SIZE,
                          .fill = 0U },
                        sd_device.file,
                        created)) {
            return false;
        }
        sd_device.sectors = linux_sd_sectors;
        runtime_storage_diagnostics.sd_blocks = linux_sd_sectors;
        if (created) {
            const UINT format_status{ fx_media_format(&sd_media,
                                                      direct_media_driver,
                                                      &sd_device,
                                                      sd_media_cache.data(),
                                                      sd_media_cache.size(),
                                                      name.data(),
                                                      1U,
                                                      ROOT_DIRECTORY_ENTRIES,
                                                      0U,
                                                      linux_sd_sectors,
                                                      SECTOR_SIZE,
                                                      8U,
                                                      1U,
                                                      1U) };
            if (format_status != FX_SUCCESS) {
                runtime_storage_diagnostics.sd_filex_status = format_status;
                return false;
            }
        }
        UINT open_status{ fx_media_open(&sd_media,
                                        name.data(),
                                        direct_media_driver,
                                        &sd_device,
                                        sd_media_cache.data(),
                                        sd_media_cache.size()) };
        runtime_storage_diagnostics.sd_filex_status = open_status;
        sd_mounted = open_status == FX_SUCCESS;
        runtime_storage_diagnostics.sd_mounted = sd_mounted;
        return sd_mounted;
    }
}

extern "C" {
runtime::storage::Diagnostics runtime_storage_diagnostics{};
}

namespace runtime::storage
{
    auto initialize() noexcept -> bool
    {
        // Startup-only initialization: a completed failure is cached too.
        if (initialization_attempted) {
            return flash_mounted || sd_mounted;
        }
        runtime_storage_diagnostics = {};
        const bool flash_ok{ initialize_flash_media() };
        const bool sd_ok{ initialize_sd_media() };
        initialization_attempted = true;
        return flash_ok || sd_ok;
    }

    auto media(Volume volume) noexcept -> FX_MEDIA*
    {
        return volume == Volume::Flash ? &flash_media : &sd_media;
    }

    auto mounted(Volume volume) noexcept -> bool
    {
        return volume == Volume::Flash ? flash_mounted : sd_mounted;
    }

    auto diagnostics() noexcept -> const Diagnostics& { return runtime_storage_diagnostics; }
}
