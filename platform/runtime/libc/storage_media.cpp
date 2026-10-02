#include "storage_media.hpp"
#include "storage_media_policy.hpp"

#include <lx_api.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ranges>

#if defined(HAL_PLATFORM_STM32)
#include "main.h"
#include "quadspi.h"
#include "sdmmc.h"
#include <stm32h7xx_hal.h>
#elif defined(HAL_PLATFORM_LINUX)
#include <unistd.h>
#endif

extern "C" UINT _fx_partition_offset_calculate(void* partition_sector,
                                               UINT partition,
                                               ULONG* partition_start,
                                               ULONG* partition_size);

#if defined(RUNTIME_HARDWARE_STORAGE_STARTUP_TEST)
extern "C" volatile std::uint32_t hardware_storage_startup_test_volume_mask;
#endif

namespace
{
    constexpr ULONG sector_size{ 512U };
    constexpr ULONG flash_bytes{ 16U * 1024U * 1024U };
    constexpr ULONG erase_block_bytes{ 4U * 1024U };
    constexpr ULONG flash_blocks{ flash_bytes / erase_block_bytes };
    constexpr ULONG flash_filex_sectors{ 24U * 1024U };
    constexpr std::size_t media_cache_bytes{ 4U * 1024U };
    constexpr std::size_t levelx_cache_bytes{ 8U * 1024U };
    constexpr std::uintptr_t levelx_base_address{ 0x9000'0000U };

    struct DirectMedia
    {
#if defined(HAL_PLATFORM_LINUX)
        std::FILE* file{};
        ULONG sectors{};
#endif
    };

    // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
    alignas(32) std::array<UCHAR, media_cache_bytes> flash_media_cache{};
    alignas(32) std::array<UCHAR, media_cache_bytes> sd_media_cache{};
    alignas(32) std::array<UCHAR, levelx_cache_bytes> levelx_cache{};
    alignas(32) std::array<ULONG, sector_size / sizeof(ULONG)> levelx_sector_buffer{};
    // Each FileX media has its own mutex, so the two physical devices must
    // also have independent scratch storage when accessed concurrently.
    alignas(32) std::array<UCHAR, sector_size> flash_verify_buffer{};
#if defined(HAL_PLATFORM_STM32)
    alignas(32) std::array<UCHAR, sector_size> sd_transfer_buffer{};
#endif
    FX_MEDIA flash_media{};
    FX_MEDIA sd_media{};
    LX_NOR_FLASH nor_flash{};
    DirectMedia sd_device{};
#if defined(HAL_PLATFORM_LINUX)
    std::FILE* nor_file{};
#endif
    bool nor_open{};
    bool initialization_attempted{};
    bool flash_mounted{};
    bool sd_mounted{};
    // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

    [[nodiscard]] auto byte_offset(const ULONG* address) noexcept -> std::size_t
    {
        return static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(address) - levelx_base_address);
    }

#if defined(HAL_PLATFORM_STM32)
    constexpr std::uint32_t qspi_timeout{ 5'000U };
    constexpr std::uint32_t w25q_write_enable{ 0x06U };
    constexpr std::uint32_t w25q_read_status_1{ 0x05U };
    constexpr std::uint32_t w25q_read_status_2{ 0x35U };
    constexpr std::uint32_t w25q_write_status{ 0x01U };
    constexpr std::uint32_t w25q_read_jedec_id{ 0x9FU };
    constexpr std::uint32_t w25q_reset_enable{ 0x66U };
    constexpr std::uint32_t w25q_reset{ 0x99U };
    constexpr std::uint32_t w25q_fast_read{ 0x0BU };
    constexpr std::uint32_t w25q_quad_page_program{ 0x32U };
    constexpr std::uint32_t w25q_sector_erase{ 0x20U };

    static_assert(runtime::storage::detail::sd_data_crc_error == HAL_SD_ERROR_DATA_CRC_FAIL);

    void configure_sd_handle(std::uint32_t bus_width) noexcept
    {
        hsd1 = {};
        hsd1.Instance = SDMMC1;
        hsd1.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
        hsd1.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
        hsd1.Init.BusWide = bus_width;
        hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
        hsd1.Init.ClockDiv = 0U;
    }

    [[nodiscard]] auto initialize_sd_card(std::uint32_t bus_width, std::uint8_t reported_bus_width) noexcept
      -> bool
    {
        configure_sd_handle(bus_width);
        runtime_storage_diagnostics.sd_bus_width = 0U;
        if (HAL_SD_Init(&hsd1) != HAL_OK) {
            runtime_storage_diagnostics.sd_error = hsd1.ErrorCode;
            runtime_storage_diagnostics.sd_blocks = hsd1.SdCard.LogBlockNbr;
            return false;
        }

        HAL_SD_CardInfoTypeDef info{};
        if (HAL_SD_GetCardInfo(&hsd1, &info) != HAL_OK) {
            runtime_storage_diagnostics.sd_error = hsd1.ErrorCode;
            runtime_storage_diagnostics.sd_blocks = hsd1.SdCard.LogBlockNbr;
            return false;
        }

        runtime_storage_diagnostics.sd_error = HAL_SD_ERROR_NONE;
        runtime_storage_diagnostics.sd_blocks = info.LogBlockNbr;
        runtime_storage_diagnostics.sd_bus_width = reported_bus_width;
        return true;
    }

    void prepare_sd_mount_retry() noexcept
    {
        static_cast<void>(HAL_SD_DeInit(&hsd1));
        hsd1 = {};
        sd_media = {};
        sd_media_cache.fill(0U);
        sd_mounted = false;
        runtime_storage_diagnostics.sd_mounted = 0U;
        runtime_storage_diagnostics.sd_bus_width = 0U;
    }

    [[nodiscard]] auto qspi_command(std::uint32_t instruction,
                                    std::uint32_t address,
                                    std::uint32_t address_mode,
                                    std::uint32_t data_mode,
                                    std::uint32_t count,
                                    std::uint32_t dummy_cycles = 0U) noexcept -> bool
    {
        QSPI_CommandTypeDef command{};
        command.Instruction = instruction;
        command.Address = address;
        command.AddressSize = QSPI_ADDRESS_24_BITS;
        command.InstructionMode = QSPI_INSTRUCTION_1_LINE;
        command.AddressMode = address_mode;
        command.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
        command.DataMode = data_mode;
        command.DummyCycles = dummy_cycles;
        command.NbData = count;
        command.DdrMode = QSPI_DDR_MODE_DISABLE;
        command.DdrHoldHalfCycle = QSPI_DDR_HHC_ANALOG_DELAY;
        command.SIOOMode = QSPI_SIOO_INST_EVERY_CMD;
        const HAL_StatusTypeDef status{ HAL_QSPI_Command(&hqspi, &command, qspi_timeout) };
        if (status != HAL_OK) {
            runtime_storage_diagnostics.qspi_error = hqspi.ErrorCode != 0U ? hqspi.ErrorCode : status;
            return false;
        }
        return true;
    }

    [[nodiscard]] auto qspi_simple(std::uint32_t instruction) noexcept -> bool
    {
        return qspi_command(instruction, 0U, QSPI_ADDRESS_NONE, QSPI_DATA_NONE, 0U);
    }

    [[nodiscard]] auto qspi_read_register(std::uint32_t instruction, UCHAR& value) noexcept -> bool
    {
        if (!qspi_command(instruction, 0U, QSPI_ADDRESS_NONE, QSPI_DATA_1_LINE, 1U)) {
            return false;
        }
        const HAL_StatusTypeDef status{ HAL_QSPI_Receive(&hqspi, &value, qspi_timeout) };
        if (status != HAL_OK) {
            runtime_storage_diagnostics.qspi_error = hqspi.ErrorCode != 0U ? hqspi.ErrorCode : status;
            return false;
        }
        return true;
    }

    [[nodiscard]] auto qspi_wait_ready() noexcept -> bool
    {
        QSPI_CommandTypeDef command{};
        command.Instruction = w25q_read_status_1;
        command.InstructionMode = QSPI_INSTRUCTION_1_LINE;
        command.AddressMode = QSPI_ADDRESS_NONE;
        command.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
        command.DataMode = QSPI_DATA_1_LINE;
        command.NbData = 1U;
        command.DdrMode = QSPI_DDR_MODE_DISABLE;
        command.DdrHoldHalfCycle = QSPI_DDR_HHC_ANALOG_DELAY;
        command.SIOOMode = QSPI_SIOO_INST_EVERY_CMD;
        QSPI_AutoPollingTypeDef polling{};
        polling.Match = 0U;
        polling.Mask = 1U;
        polling.Interval = 16U;
        polling.StatusBytesSize = 1U;
        polling.MatchMode = QSPI_MATCH_MODE_AND;
        polling.AutomaticStop = QSPI_AUTOMATIC_STOP_ENABLE;
        const HAL_StatusTypeDef status{ HAL_QSPI_AutoPolling(&hqspi, &command, &polling, qspi_timeout) };
        if (status != HAL_OK) {
            runtime_storage_diagnostics.qspi_error = hqspi.ErrorCode != 0U ? hqspi.ErrorCode : status;
            return false;
        }
        return true;
    }

    [[nodiscard]] auto qspi_write_enable() noexcept -> bool { return qspi_simple(w25q_write_enable); }

    [[nodiscard]] auto qspi_read(std::size_t offset, void* destination, std::size_t bytes) noexcept -> bool
    {
        if (offset > flash_bytes || bytes > flash_bytes - offset || bytes == 0U) {
            return bytes == 0U;
        }
        // Keep reads in single-line fast-read mode: this is the mode verified
        // to be reliable on the wired breakout. Quad page programming remains
        // enabled and is verified by LevelX readback.
        if (!qspi_command(w25q_fast_read,
                          static_cast<std::uint32_t>(offset),
                          QSPI_ADDRESS_1_LINE,
                          QSPI_DATA_1_LINE,
                          static_cast<std::uint32_t>(bytes),
                          8U)) {
            return false;
        }
        const HAL_StatusTypeDef status{ HAL_QSPI_Receive(
          &hqspi, static_cast<UCHAR*>(destination), qspi_timeout) };
        if (status != HAL_OK) {
            runtime_storage_diagnostics.qspi_error = hqspi.ErrorCode != 0U ? hqspi.ErrorCode : status;
            return false;
        }
        return true;
    }

    [[nodiscard]] auto qspi_write(std::size_t offset, const void* source, std::size_t bytes) noexcept -> bool
    {
        const auto* input{ static_cast<const UCHAR*>(source) };
        if (offset > flash_bytes || bytes > flash_bytes - offset) {
            return false;
        }
        while (bytes != 0U) {
            const std::size_t page_bytes{ std::min<std::size_t>(bytes, 256U - (offset & 0xFFU)) };
            if (!qspi_write_enable() || !qspi_command(w25q_quad_page_program,
                                                      static_cast<std::uint32_t>(offset),
                                                      QSPI_ADDRESS_1_LINE,
                                                      QSPI_DATA_4_LINES,
                                                      static_cast<std::uint32_t>(page_bytes))) {
                return false;
            }
            const HAL_StatusTypeDef status{ HAL_QSPI_Transmit(
              &hqspi, const_cast<UCHAR*>(input), qspi_timeout) };
            if (status != HAL_OK || !qspi_wait_ready()) {
                if (status != HAL_OK) {
                    runtime_storage_diagnostics.qspi_error = hqspi.ErrorCode != 0U ? hqspi.ErrorCode : status;
                }
                return false;
            }
            offset += page_bytes;
            input += page_bytes;
            bytes -= page_bytes;
        }
        return true;
    }

    [[nodiscard]] auto qspi_erase(std::size_t offset) noexcept -> bool
    {
        return offset < flash_bytes && (offset % erase_block_bytes) == 0U && qspi_write_enable() &&
               qspi_command(w25q_sector_erase,
                            static_cast<std::uint32_t>(offset),
                            QSPI_ADDRESS_1_LINE,
                            QSPI_DATA_NONE,
                            0U) &&
               qspi_wait_ready();
    }

    [[nodiscard]] auto initialize_qspi() noexcept -> bool
    {
        MX_QUADSPI_Init();
        if (!qspi_simple(w25q_reset_enable) || !qspi_simple(w25q_reset)) {
            return false;
        }
        HAL_Delay(1U);

        std::array<UCHAR, 3U> id{};
        if (!qspi_command(w25q_read_jedec_id, 0U, QSPI_ADDRESS_NONE, QSPI_DATA_1_LINE, id.size()) ||
            HAL_QSPI_Receive(&hqspi, id.data(), qspi_timeout) != HAL_OK) {
            runtime_storage_diagnostics.qspi_error = hqspi.ErrorCode;
            return false;
        }
        runtime_storage_diagnostics.flash_jedec_id =
          (static_cast<std::uint32_t>(id[0]) << 16U) | (static_cast<std::uint32_t>(id[1]) << 8U) | id[2];
        if (id != std::array<UCHAR, 3U>{ 0xEFU, 0x40U, 0x18U }) {
            runtime_storage_diagnostics.qspi_error = 0xBAD0'0001U;
            return false;
        }

        UCHAR status_1{};
        UCHAR status_2{};
        if (!qspi_read_register(w25q_read_status_1, status_1) ||
            !qspi_read_register(w25q_read_status_2, status_2)) {
            return false;
        }
        if ((status_2 & 0x02U) == 0U) {
            std::array<UCHAR, 2U> statuses{ status_1, static_cast<UCHAR>(status_2 | 0x02U) };
            if (!qspi_write_enable() ||
                !qspi_command(w25q_write_status, 0U, QSPI_ADDRESS_NONE, QSPI_DATA_1_LINE, statuses.size()) ||
                HAL_QSPI_Transmit(&hqspi, statuses.data(), qspi_timeout) != HAL_OK || !qspi_wait_ready() ||
                !qspi_read_register(w25q_read_status_2, status_2) || (status_2 & 0x02U) == 0U) {
                runtime_storage_diagnostics.qspi_error = hqspi.ErrorCode;
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] auto sd_wait_ready() noexcept -> bool
    {
        const std::uint32_t started{ HAL_GetTick() };
        while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER) {
            if (HAL_GetTick() - started >= 5'000U) {
                runtime_storage_diagnostics.sd_error =
                  hsd1.ErrorCode != HAL_SD_ERROR_NONE ? hsd1.ErrorCode : HAL_SD_ERROR_TIMEOUT;
                return false;
            }
            tx_thread_sleep(1U);
        }
        return true;
    }

    [[nodiscard]] auto sd_read(ULONG sector, void* destination, UINT sectors) noexcept -> bool
    {
        if (!runtime::storage::detail::valid_sector_range(sector, 0U, sectors, hsd1.SdCard.LogBlockNbr)) {
            runtime_storage_diagnostics.sd_error = HAL_SD_ERROR_ADDR_OUT_OF_RANGE;
            return false;
        }
        auto* output{ static_cast<UCHAR*>(destination) };
        for (UINT index{}; index < sectors; ++index) {
            if (HAL_SD_ReadBlocks(
                  &hsd1, sd_transfer_buffer.data(), static_cast<std::uint32_t>(sector + index), 1U, 5'000U) !=
                  HAL_OK) {
                runtime_storage_diagnostics.sd_error = hsd1.ErrorCode;
                return false;
            }
            if (!sd_wait_ready()) {
                return false;
            }
            std::memcpy(output + index * sector_size, sd_transfer_buffer.data(), sector_size);
        }
        return true;
    }

    [[nodiscard]] auto sd_write(ULONG sector, const void* source, UINT sectors) noexcept -> bool
    {
        if (!runtime::storage::detail::valid_sector_range(sector, 0U, sectors, hsd1.SdCard.LogBlockNbr)) {
            runtime_storage_diagnostics.sd_error = HAL_SD_ERROR_ADDR_OUT_OF_RANGE;
            return false;
        }
        const auto* input{ static_cast<const UCHAR*>(source) };
        for (UINT index{}; index < sectors; ++index) {
            std::memcpy(sd_transfer_buffer.data(), input + index * sector_size, sector_size);
            if (HAL_SD_WriteBlocks(
                  &hsd1, sd_transfer_buffer.data(), static_cast<std::uint32_t>(sector + index), 1U, 5'000U) !=
                  HAL_OK) {
                runtime_storage_diagnostics.sd_error = hsd1.ErrorCode;
                return false;
            }
            if (!sd_wait_ready()) {
                return false;
            }
        }
        return true;
    }
#else
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

    [[nodiscard]] auto open_image(const char* kind,
                                  std::size_t bytes,
                                  UCHAR fill,
                                  std::FILE*& file,
                                  bool& created) noexcept -> bool
    {
        std::array<char, 128U> path{};
        const int length{ std::snprintf(
          path.data(), path.size(), "/tmp/nucleo-h753zi-%ld-%s.img", static_cast<long>(getpid()), kind) };
        if (length <= 0 || static_cast<std::size_t>(length) >= path.size()) {
            return false;
        }
        file = std::fopen(path.data(), "r+b");
        created = file == nullptr;
        if (created) {
            file = std::fopen(path.data(), "w+b");
        }
        if (file == nullptr) {
            return false;
        }
        if (created) {
            std::array<UCHAR, erase_block_bytes> block{};
            block.fill(fill);
            for (std::size_t offset{}; offset < bytes; offset += block.size()) {
                const std::size_t count{ std::min(block.size(), bytes - offset) };
                if (std::fwrite(block.data(), 1U, count, file) != count) {
                    return false;
                }
            }
            if (std::fflush(file) != 0) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] auto qspi_read(std::size_t offset, void* destination, std::size_t bytes) noexcept -> bool
    {
        return offset <= flash_bytes && bytes <= flash_bytes - offset &&
               file_read(nor_file, offset, destination, bytes);
    }

    [[nodiscard]] auto qspi_write(std::size_t offset, const void* source, std::size_t bytes) noexcept -> bool
    {
        if (offset > flash_bytes || bytes > flash_bytes - offset) {
            return false;
        }
        std::array<UCHAR, 256U> current{};
        const auto* input{ static_cast<const UCHAR*>(source) };
        while (bytes != 0U) {
            const std::size_t count{ std::min(bytes, current.size()) };
            if (!file_read(nor_file, offset, current.data(), count)) {
                return false;
            }
            for (std::size_t index{}; index < count; ++index) {
                if ((current[index] & input[index]) != input[index]) {
                    return false;
                }
            }
            if (!file_write(nor_file, offset, input, count)) {
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
        std::array<UCHAR, erase_block_bytes> erased{};
        erased.fill(0xFFU);
        return offset < flash_bytes && (offset % erase_block_bytes) == 0U &&
               file_write(nor_file, offset, erased.data(), erased.size());
    }

    [[nodiscard]] auto sd_read(ULONG sector, void* destination, UINT sectors) noexcept -> bool
    {
        return runtime::storage::detail::valid_sector_range(sector, 0U, sectors, sd_device.sectors) &&
               file_read(sd_device.file,
                         static_cast<std::size_t>(sector) * sector_size,
                         destination,
                         static_cast<std::size_t>(sectors) * sector_size);
    }

    [[nodiscard]] auto sd_write(ULONG sector, const void* source, UINT sectors) noexcept -> bool
    {
        return runtime::storage::detail::valid_sector_range(sector, 0U, sectors, sd_device.sectors) &&
               file_write(sd_device.file,
                          static_cast<std::size_t>(sector) * sector_size,
                          source,
                          static_cast<std::size_t>(sectors) * sector_size);
    }
#endif

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
                if (runtime::storage::detail::valid_sector_range(
                      media_ptr->fx_media_driver_logical_sector,
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
                if (runtime::storage::detail::valid_sector_range(
                      media_ptr->fx_media_driver_logical_sector,
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

    [[nodiscard]] auto nor_driver_erase(ULONG block, ULONG) noexcept -> UINT
    {
        return block < flash_blocks && qspi_erase(static_cast<std::size_t>(block) * erase_block_bytes)
                 ? LX_SUCCESS
                 : LX_ERROR;
    }

    [[nodiscard]] auto nor_driver_verify(ULONG block) noexcept -> UINT
    {
        if (block >= flash_blocks) {
            return LX_ERROR;
        }
        for (std::size_t offset{}; offset < erase_block_bytes; offset += flash_verify_buffer.size()) {
            if (!qspi_read(static_cast<std::size_t>(block) * erase_block_bytes + offset,
                           flash_verify_buffer.data(),
                           flash_verify_buffer.size()) ||
                !std::ranges::all_of(flash_verify_buffer, [](UCHAR byte) { return byte == 0xFFU; })) {
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
        flash->lx_nor_flash_total_blocks = flash_blocks;
        flash->lx_nor_flash_words_per_block = erase_block_bytes / sizeof(ULONG);
        flash->lx_nor_flash_base_address = reinterpret_cast<ULONG*>(levelx_base_address);
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
        UINT status{ lx_nor_flash_open(&nor_flash, const_cast<CHAR*>("W25Q128"), nor_driver_initialize) };
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

    auto levelx_media_driver(FX_MEDIA* media_ptr) noexcept -> void
    {
        media_ptr->fx_media_driver_status = FX_IO_ERROR;
        UINT status{ LX_SUCCESS };
        switch (media_ptr->fx_media_driver_request) {
            case FX_DRIVER_INIT:
                status = open_levelx();
                if (status == LX_SUCCESS) {
                    media_ptr->fx_media_driver_free_sector_update = FX_TRUE;
                    media_ptr->fx_media_driver_status = FX_SUCCESS;
                }
                return;
            case FX_DRIVER_UNINIT:
                media_ptr->fx_media_driver_status = close_levelx() == LX_SUCCESS ? FX_SUCCESS : FX_IO_ERROR;
                return;
            case FX_DRIVER_READ:
            case FX_DRIVER_BOOT_READ: {
                const bool boot{ media_ptr->fx_media_driver_request == FX_DRIVER_BOOT_READ };
                ULONG sector{ boot ? 0U : media_ptr->fx_media_driver_logical_sector };
                auto* destination{ static_cast<UCHAR*>(media_ptr->fx_media_driver_buffer) };
                const UINT sectors{ boot ? 1U : media_ptr->fx_media_driver_sectors };
                if (!runtime::storage::detail::valid_sector_range(sector, 0U, sectors, flash_filex_sectors)) {
                    return;
                }
                for (UINT index{}; index < sectors; ++index) {
                    status = lx_nor_flash_sector_read(&nor_flash, sector + index, destination);
                    if (status != LX_SUCCESS) {
                        return;
                    }
                    destination += sector_size;
                }
                media_ptr->fx_media_driver_status = FX_SUCCESS;
                return;
            }
            case FX_DRIVER_WRITE:
            case FX_DRIVER_BOOT_WRITE: {
                const bool boot{ media_ptr->fx_media_driver_request == FX_DRIVER_BOOT_WRITE };
                ULONG sector{ boot ? 0U : media_ptr->fx_media_driver_logical_sector };
                auto* source{ static_cast<UCHAR*>(media_ptr->fx_media_driver_buffer) };
                const UINT sectors{ boot ? 1U : media_ptr->fx_media_driver_sectors };
                if (!runtime::storage::detail::valid_sector_range(sector, 0U, sectors, flash_filex_sectors)) {
                    return;
                }
                for (UINT index{}; index < sectors; ++index) {
                    status = lx_nor_flash_sector_write(&nor_flash, sector + index, source);
                    if (status != LX_SUCCESS) {
                        return;
                    }
                    source += sector_size;
                }
                media_ptr->fx_media_driver_status = FX_SUCCESS;
                return;
            }
            case FX_DRIVER_RELEASE_SECTORS:
                if (!runtime::storage::detail::valid_sector_range(media_ptr->fx_media_driver_logical_sector,
                                                                   0U,
                                                                   media_ptr->fx_media_driver_sectors,
                                                                   flash_filex_sectors)) {
                    return;
                }
                for (UINT index{}; index < media_ptr->fx_media_driver_sectors; ++index) {
                    status = lx_nor_flash_sector_release(&nor_flash,
                                                         media_ptr->fx_media_driver_logical_sector + index);
                    if (status != LX_SUCCESS && status != LX_SECTOR_NOT_FOUND) {
                        return;
                    }
                }
                media_ptr->fx_media_driver_status = FX_SUCCESS;
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
#if defined(HAL_PLATFORM_STM32)
        if (!initialize_qspi()) {
            return false;
        }
#if defined(RUNTIME_STORAGE_ERASE_FLASH_ON_BOOT)
        for (ULONG block{}; block < flash_blocks; ++block) {
            if (!qspi_erase(static_cast<std::size_t>(block) * erase_block_bytes)) {
                return false;
            }
        }
#endif
#else
        bool created{};
        if (!open_image("flash-levelx", flash_bytes, 0xFFU, nor_file, created)) {
            return false;
        }
        runtime_storage_diagnostics.flash_jedec_id = 0xEF'40'18U;
#endif
        runtime_storage_diagnostics.flash_blocks = flash_blocks;
        lx_nor_flash_initialize();
        if (open_levelx() != LX_SUCCESS) {
            return false;
        }
        runtime_storage_diagnostics.flash_logical_sectors = flash_filex_sectors;

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
                                                      const_cast<CHAR*>("FLASH"),
                                                      1U,
                                                      128U,
                                                      0U,
                                                      flash_filex_sectors,
                                                      sector_size,
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
                                              const_cast<CHAR*>("FLASH"),
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
#if defined(HAL_PLATFORM_STM32)
        runtime_storage_diagnostics.sd_detect_level =
          HAL_GPIO_ReadPin(SD_CARD_DETECT_GPIO_Port, SD_CARD_DETECT_Pin) == GPIO_PIN_SET ? 1U : 0U;
        if (!initialize_sd_card(SDMMC_BUS_WIDE_4B, 4U)) {
            const std::uint32_t four_bit_error{ hsd1.ErrorCode };
            const std::uint32_t four_bit_blocks{ hsd1.SdCard.LogBlockNbr };
            runtime_storage_diagnostics.sd_fallback_error = four_bit_error;
            runtime_storage_diagnostics.sd_fallback_used = 1U;
            static_cast<void>(HAL_SD_DeInit(&hsd1));
            if (!initialize_sd_card(SDMMC_BUS_WIDE_1B, 1U)) {
                if (runtime_storage_diagnostics.sd_blocks == 0U) {
                    runtime_storage_diagnostics.sd_blocks = four_bit_blocks;
                }
                return false;
            }
        }
#else
        constexpr ULONG linux_sd_sectors{ 128U * 1024U };
        bool created{};
        if (!open_image("sd-filex",
                        static_cast<std::size_t>(linux_sd_sectors) * sector_size,
                        0U,
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
                                                      const_cast<CHAR*>("SD TEST"),
                                                      1U,
                                                      128U,
                                                      0U,
                                                      linux_sd_sectors,
                                                      sector_size,
                                                      8U,
                                                      1U,
                                                      1U) };
            if (format_status != FX_SUCCESS) {
                runtime_storage_diagnostics.sd_filex_status = format_status;
                return false;
            }
        }
#endif
        UINT open_status{ fx_media_open(&sd_media,
                                        const_cast<CHAR*>("SD"),
                                        direct_media_driver,
                                        &sd_device,
                                        sd_media_cache.data(),
                                        sd_media_cache.size()) };
#if defined(HAL_PLATFORM_STM32)
        if (runtime::storage::detail::should_retry_sd_mount_in_one_bit(
              runtime_storage_diagnostics.sd_bus_width, open_status, runtime_storage_diagnostics.sd_error)) {
            const std::uint32_t four_bit_error{ runtime_storage_diagnostics.sd_error };
            const std::uint32_t four_bit_blocks{ runtime_storage_diagnostics.sd_blocks };
            runtime_storage_diagnostics.sd_fallback_error = four_bit_error;
            runtime_storage_diagnostics.sd_fallback_used = 1U;
            prepare_sd_mount_retry();
            if (!initialize_sd_card(SDMMC_BUS_WIDE_1B, 1U)) {
                if (runtime_storage_diagnostics.sd_blocks == 0U) {
                    runtime_storage_diagnostics.sd_blocks = four_bit_blocks;
                }
                runtime_storage_diagnostics.sd_filex_status = open_status;
                return false;
            }
            open_status = fx_media_open(&sd_media,
                                        const_cast<CHAR*>("SD"),
                                        direct_media_driver,
                                        &sd_device,
                                        sd_media_cache.data(),
                                        sd_media_cache.size());
        }
#endif
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
#if defined(RUNTIME_HARDWARE_STORAGE_STARTUP_TEST)
        const bool flash_ok{ (hardware_storage_startup_test_volume_mask & 1U) != 0U && initialize_flash_media() };
        const bool sd_ok{ (hardware_storage_startup_test_volume_mask & 2U) != 0U && initialize_sd_media() };
#else
        const bool flash_ok{ initialize_flash_media() };
        const bool sd_ok{ initialize_sd_media() };
#endif
        initialization_attempted = true;
        return flash_ok || sd_ok;
    }

    auto media(Volume volume) noexcept -> FX_MEDIA*
    {
        return volume == Volume::flash ? &flash_media : &sd_media;
    }

    auto mounted(Volume volume) noexcept -> bool
    {
        return volume == Volume::flash ? flash_mounted : sd_mounted;
    }

    auto diagnostics() noexcept -> const Diagnostics& { return runtime_storage_diagnostics; }
}
