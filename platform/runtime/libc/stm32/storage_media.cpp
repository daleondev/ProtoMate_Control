#include "runtime/libc/storage_media.hpp"

extern "C" {
    runtime::storage::Diagnostics runtime_storage_diagnostics{};
}

#if defined(CORE_CM7)
#include "InternalFlash.hpp"
#include "runtime/libc/storage_media_policy.hpp"

#include <array>
#include <span>

namespace
{
    using runtime::storage::FlashJournal;
    runtime::storage::InternalFlash device;
    // CPU-only storage state: no DMA accesses these objects. Use private DTCM
    // instead of consuming the AXI RAM needed by the CLI and ICC thread heaps.
    [[gnu::section(".dtcm_bss")]] FlashJournal journal{ device };
    [[gnu::section(".dtcm_bss")]] FX_MEDIA flash{};
    [[gnu::section(".dtcm_bss")]] alignas(32) std::array<UCHAR, 4096> cache{};
    [[gnu::section(".dtcm_bss")]] alignas(32) std::array<UCHAR, 4096> recovery{};
    bool attempted{};
    bool mounted{};

    void driver(FX_MEDIA* media) noexcept
    {
        auto& status = media->fx_media_driver_status;
        status = FX_IO_ERROR;
        switch (media->fx_media_driver_request) {
            case FX_DRIVER_INIT:
                media->fx_media_driver_free_sector_update = FX_TRUE;
                status = FX_SUCCESS;
                return;
            case FX_DRIVER_UNINIT:
            case FX_DRIVER_FLUSH:
            case FX_DRIVER_ABORT:
                status = FX_SUCCESS; // Every sector write is committed before returning.
                return;
            case FX_DRIVER_READ:
            case FX_DRIVER_WRITE:
            case FX_DRIVER_BOOT_READ:
            case FX_DRIVER_BOOT_WRITE:
            case FX_DRIVER_RELEASE_SECTORS: break;
            default: return;
        }
        const auto request = media->fx_media_driver_request;
        const bool boot = request == FX_DRIVER_BOOT_READ || request == FX_DRIVER_BOOT_WRITE;
        const bool write = request == FX_DRIVER_WRITE || request == FX_DRIVER_BOOT_WRITE;
        const auto sector = boot ? 0UL : media->fx_media_driver_logical_sector;
        const auto count = boot ? 1U : media->fx_media_driver_sectors;
        if (!runtime::storage::detail::valid_sector_range(sector, 0, count, FlashJournal::SECTORS)) { return; }
        auto* buffer = reinterpret_cast<std::byte*>(media->fx_media_driver_buffer);
        for (std::size_t i{}; i < count; ++i) {
            bool ok{};
            if (request == FX_DRIVER_RELEASE_SECTORS) { ok = journal.release(sector + i); }
            else {
                auto bytes = std::span<std::byte, FlashJournal::SECTOR_BYTES>{ buffer + i * FlashJournal::SECTOR_BYTES, FlashJournal::SECTOR_BYTES };
                ok = write ? journal.write(sector + i, bytes) : journal.read(sector + i, bytes);
            }
            runtime_storage_diagnostics.internal_flash_error = device.error();
            runtime_storage_diagnostics.flash_erase_operations = journal.erases();
            if (!ok) { return; }
        }
        status = FX_SUCCESS;
    }
}

namespace runtime::storage
{
    bool initialize() noexcept
    {
        if (attempted) { return ::mounted; }
        attempted = true;
        auto& info = runtime_storage_diagnostics;
        info.flash_blocks = FlashJournal::BLOCKS;
        info.flash_logical_sectors = FlashJournal::SECTORS;
        const auto state = journal.mount();
        info.internal_flash_error = device.error();
        if (state == FlashJournal::Mount::Corrupt) { info.flash_filex_status = FX_MEDIA_INVALID; return false; }
        static auto name = std::to_array("FLASH");
        if (state == FlashJournal::Mount::Blank) {
            info.flash_filex_status = fx_media_format(&flash, driver, nullptr, cache.data(), cache.size(),
                name.data(), 1, 128, 0, FlashJournal::SECTORS, FlashJournal::SECTOR_BYTES, 1, 1, 1);
            if (info.flash_filex_status != FX_SUCCESS) { return false; }
            info.flash_was_formatted = 1;
        }
        info.flash_filex_status = fx_media_open(&flash, name.data(), driver, nullptr, cache.data(), cache.size());
        if (info.flash_filex_status != FX_SUCCESS) { return false; }
        info.flash_filex_status = fx_fault_tolerant_enable(&flash, recovery.data(), recovery.size());
        if (info.flash_filex_status != FX_SUCCESS) { static_cast<void>(fx_media_close(&flash)); return false; }
        ::mounted = true;
        info.flash_mounted = 1;
        return true;
    }
    FX_MEDIA* media(Volume volume) noexcept { return volume == Volume::Flash && ::mounted ? &flash : nullptr; }
    bool mounted(Volume volume) noexcept { return media(volume) != nullptr; }
    const Diagnostics& diagnostics() noexcept { return runtime_storage_diagnostics; }
}
#else
// M7 owns the physical volume. M4 must not mount a second FileX instance on it.
namespace runtime::storage
{
    bool initialize() noexcept { return false; }
    FX_MEDIA* media(Volume) noexcept { return nullptr; }
    bool mounted(Volume) noexcept { return false; }
    const Diagnostics& diagnostics() noexcept { return runtime_storage_diagnostics; }
}
#endif
