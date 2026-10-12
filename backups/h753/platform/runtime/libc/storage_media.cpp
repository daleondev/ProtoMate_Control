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

#if defined(HAL_PLATFORM_STM32)
#include "main.h"
#include "quadspi.h"
#include "sdmmc.h"
#include <stm32h7xx_hal.h>
#elif defined(HAL_PLATFORM_LINUX)
#include <unistd.h>
#endif

// FileX exports this ABI symbol.
// NOLINTNEXTLINE(bugprone-reserved-identifier,readability-identifier-naming)
extern "C" UINT _fx_partition_offset_calculate(void* partition_sector,
                                               UINT partition,
                                               ULONG* partition_start,
                                               ULONG* partition_size);

#if defined(RUNTIME_HARDWARE_STORAGE_STARTUP_TEST)
extern "C" volatile std::uint32_t hardware_storage_startup_test_volume_mask;
#endif

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

#if defined(HAL_PLATFORM_LINUX)
    struct FileCloser
    {
        // Invoked by unique_ptr, which transfers ownership to its deleter.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        void operator()(std::FILE* file) const noexcept { static_cast<void>(std::fclose(file)); }
    };
    using File = std::unique_ptr<std::FILE, FileCloser>;
#endif

    struct DirectMedia
    {
#if defined(HAL_PLATFORM_LINUX)
        File file;
        ULONG sectors{};
#endif
    };

    // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
    alignas(CACHE_LINE_BYTES) std::array<UCHAR, MEDIA_CACHE_BYTES> flash_media_cache{};
    alignas(CACHE_LINE_BYTES) std::array<UCHAR, MEDIA_CACHE_BYTES> sd_media_cache{};
    alignas(CACHE_LINE_BYTES) std::array<UCHAR, LEVELX_CACHE_BYTES> levelx_cache{};
    alignas(CACHE_LINE_BYTES) std::array<ULONG, SECTOR_SIZE / sizeof(ULONG)> levelx_sector_buffer{};
    // Each FileX media has its own mutex, so the two physical devices must
    // also have independent scratch storage when accessed concurrently.
    alignas(CACHE_LINE_BYTES) std::array<UCHAR, SECTOR_SIZE> flash_verify_buffer{};
#if defined(HAL_PLATFORM_STM32)
    alignas(CACHE_LINE_BYTES) std::array<UCHAR, SECTOR_SIZE> sd_transfer_buffer{};
#endif
    FX_MEDIA flash_media{};
    FX_MEDIA sd_media{};
    LX_NOR_FLASH nor_flash{};
    DirectMedia sd_device{};
#if defined(HAL_PLATFORM_LINUX)
    File nor_file{};
#endif
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

#if defined(HAL_PLATFORM_STM32)
    constexpr std::uint32_t QSPI_TIMEOUT{ 5'000U };
    constexpr std::uint32_t SD_TIMEOUT{ 5'000U };
    constexpr std::uint32_t FLASH_POLL_INTERVAL{ 16U };
    constexpr std::uint32_t FAST_READ_DUMMY_CYCLES{ 8U };
    constexpr std::uint32_t UNKNOWN_FLASH_ID_ERROR{ 0xBAD00001U };
    enum class SdBusWidth : std::uint8_t
    {
        One,
        Four
    };
    constexpr std::uint32_t W25Q_WRITE_ENABLE{ 0x06U };
    constexpr std::uint32_t W25Q_READ_STATUS_1{ 0x05U };
    constexpr std::uint32_t W25Q_READ_STATUS_2{ 0x35U };
    constexpr std::uint32_t W25Q_WRITE_STATUS{ 0x01U };
    constexpr std::uint32_t W25Q_READ_JEDEC_ID{ 0x9FU };
    constexpr std::uint32_t W25Q_RESET_ENABLE{ 0x66U };
    constexpr std::uint32_t W25Q_RESET{ 0x99U };
    constexpr std::uint32_t W25Q_FAST_READ{ 0x0BU };
    constexpr std::uint32_t W25Q_QUAD_PAGE_PROGRAM{ 0x32U };
    constexpr std::uint32_t W25Q_SECTOR_ERASE{ 0x20U };

    static_assert(runtime::storage::detail::SD_DATA_CRC_ERROR == HAL_SD_ERROR_DATA_CRC_FAIL);

    void configure_sd_handle(std::uint32_t bus_width) noexcept
    {
        hsd1 = {};
        // CMSIS peripheral addresses are fixed by the STM32 memory map.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
        hsd1.Instance = SDMMC1;
        hsd1.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
        hsd1.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
        hsd1.Init.BusWide = bus_width;
        hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
        hsd1.Init.ClockDiv = 0U;
    }

    [[nodiscard]] auto initialize_sd_card(SdBusWidth width) noexcept -> bool
    {
        configure_sd_handle(width == SdBusWidth::Four ? SDMMC_BUS_WIDE_4B : SDMMC_BUS_WIDE_1B);
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
        runtime_storage_diagnostics.sd_bus_width = width == SdBusWidth::Four ? 4U : 1U;
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

    struct QspiTransfer
    {
        std::uint32_t instruction{}, address{}, address_mode{}, data_mode{}, count{}, dummy_cycles{};
    };

    [[nodiscard]] auto qspi_command(const QspiTransfer& transfer) noexcept -> bool
    {
        QSPI_CommandTypeDef command{};
        command.Instruction = transfer.instruction;
        command.Address = transfer.address;
        command.AddressSize = QSPI_ADDRESS_24_BITS;
        command.InstructionMode = QSPI_INSTRUCTION_1_LINE;
        command.AddressMode = transfer.address_mode;
        command.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
        command.DataMode = transfer.data_mode;
        command.DummyCycles = transfer.dummy_cycles;
        command.NbData = transfer.count;
        command.DdrMode = QSPI_DDR_MODE_DISABLE;
        command.DdrHoldHalfCycle = QSPI_DDR_HHC_ANALOG_DELAY;
        command.SIOOMode = QSPI_SIOO_INST_EVERY_CMD;
        const HAL_StatusTypeDef status{ HAL_QSPI_Command(&hqspi, &command, QSPI_TIMEOUT) };
        if (status != HAL_OK) {
            runtime_storage_diagnostics.qspi_error =
              hqspi.ErrorCode != 0U ? hqspi.ErrorCode : static_cast<std::uint32_t>(status);
            return false;
        }
        return true;
    }

    [[nodiscard]] auto qspi_simple(std::uint32_t instruction) noexcept -> bool
    {
        return qspi_command({ .instruction = instruction,
                              .address = 0U,
                              .address_mode = QSPI_ADDRESS_NONE,
                              .data_mode = QSPI_DATA_NONE,
                              .count = 0U });
    }

    [[nodiscard]] auto qspi_read_register(std::uint32_t instruction, UCHAR& value) noexcept -> bool
    {
        if (!qspi_command({ .instruction = instruction,
                            .address = 0U,
                            .address_mode = QSPI_ADDRESS_NONE,
                            .data_mode = QSPI_DATA_1_LINE,
                            .count = 1U })) {
            return false;
        }
        const HAL_StatusTypeDef status{ HAL_QSPI_Receive(&hqspi, &value, QSPI_TIMEOUT) };
        if (status != HAL_OK) {
            runtime_storage_diagnostics.qspi_error =
              hqspi.ErrorCode != 0U ? hqspi.ErrorCode : static_cast<std::uint32_t>(status);
            return false;
        }
        return true;
    }

    [[nodiscard]] auto qspi_wait_ready() noexcept -> bool
    {
        QSPI_CommandTypeDef command{};
        command.Instruction = W25Q_READ_STATUS_1;
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
        polling.Interval = FLASH_POLL_INTERVAL;
        polling.StatusBytesSize = 1U;
        polling.MatchMode = QSPI_MATCH_MODE_AND;
        polling.AutomaticStop = QSPI_AUTOMATIC_STOP_ENABLE;
        const HAL_StatusTypeDef status{ HAL_QSPI_AutoPolling(&hqspi, &command, &polling, QSPI_TIMEOUT) };
        if (status != HAL_OK) {
            runtime_storage_diagnostics.qspi_error =
              hqspi.ErrorCode != 0U ? hqspi.ErrorCode : static_cast<std::uint32_t>(status);
            return false;
        }
        return true;
    }

    [[nodiscard]] auto qspi_write_enable() noexcept -> bool { return qspi_simple(W25Q_WRITE_ENABLE); }

    [[nodiscard]] auto qspi_read(std::size_t offset, void* destination, std::size_t bytes) noexcept -> bool
    {
        if (offset > FLASH_BYTES || bytes > FLASH_BYTES - offset || bytes == 0U) {
            return bytes == 0U;
        }
        // Keep reads in single-line fast-read mode: this is the mode verified
        // to be reliable on the wired breakout. Quad page programming remains
        // enabled and is verified by LevelX readback.
        if (!qspi_command({ .instruction = W25Q_FAST_READ,
                            .address = static_cast<std::uint32_t>(offset),
                            .address_mode = QSPI_ADDRESS_1_LINE,
                            .data_mode = QSPI_DATA_1_LINE,
                            .count = static_cast<std::uint32_t>(bytes),
                            .dummy_cycles = FAST_READ_DUMMY_CYCLES })) {
            return false;
        }
        const HAL_StatusTypeDef status{ HAL_QSPI_Receive(
          &hqspi, static_cast<UCHAR*>(destination), QSPI_TIMEOUT) };
        if (status != HAL_OK) {
            runtime_storage_diagnostics.qspi_error =
              hqspi.ErrorCode != 0U ? hqspi.ErrorCode : static_cast<std::uint32_t>(status);
            return false;
        }
        return true;
    }

    [[nodiscard]] auto qspi_write(std::size_t offset, const void* source, std::size_t bytes) noexcept -> bool
    {
        const auto* input{ static_cast<const UCHAR*>(source) };
        if (offset > FLASH_BYTES || bytes > FLASH_BYTES - offset) {
            return false;
        }
        while (bytes != 0U) {
            const std::size_t page_bytes{ std::min<std::size_t>(
              bytes, FLASH_PAGE_BYTES - (offset % FLASH_PAGE_BYTES)) };
            if (!qspi_write_enable() || !qspi_command({ .instruction = W25Q_QUAD_PAGE_PROGRAM,
                                                        .address = static_cast<std::uint32_t>(offset),
                                                        .address_mode = QSPI_ADDRESS_1_LINE,
                                                        .data_mode = QSPI_DATA_4_LINES,
                                                        .count = static_cast<std::uint32_t>(page_bytes) })) {
                return false;
            }
            // HAL transmits from this buffer without modifying it.
            // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast)
            const HAL_StatusTypeDef status{ HAL_QSPI_Transmit(
              &hqspi, const_cast<UCHAR*>(input), QSPI_TIMEOUT) };
            // NOLINTEND(cppcoreguidelines-pro-type-const-cast)
            if (status != HAL_OK || !qspi_wait_ready()) {
                if (status != HAL_OK) {
                    runtime_storage_diagnostics.qspi_error =
                      hqspi.ErrorCode != 0U ? hqspi.ErrorCode : static_cast<std::uint32_t>(status);
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
        return offset < FLASH_BYTES && (offset % ERASE_BLOCK_BYTES) == 0U && qspi_write_enable() &&
               qspi_command({ .instruction = W25Q_SECTOR_ERASE,
                              .address = static_cast<std::uint32_t>(offset),
                              .address_mode = QSPI_ADDRESS_1_LINE,
                              .data_mode = QSPI_DATA_NONE,
                              .count = 0U }) &&
               qspi_wait_ready();
    }

    [[nodiscard]] auto initialize_qspi() noexcept -> bool
    {
        MX_QUADSPI_Init();
        if (!qspi_simple(W25Q_RESET_ENABLE) || !qspi_simple(W25Q_RESET)) {
            return false;
        }
        HAL_Delay(1U);

        std::array<UCHAR, 3U> id{};
        if (!qspi_command({ .instruction = W25Q_READ_JEDEC_ID,
                            .address = 0U,
                            .address_mode = QSPI_ADDRESS_NONE,
                            .data_mode = QSPI_DATA_1_LINE,
                            .count = id.size() }) ||
            HAL_QSPI_Receive(&hqspi, id.data(), QSPI_TIMEOUT) != HAL_OK) {
            runtime_storage_diagnostics.qspi_error = hqspi.ErrorCode;
            return false;
        }
        runtime_storage_diagnostics.flash_jedec_id = (static_cast<std::uint32_t>(id[0]) << (2U * CHAR_BIT)) |
                                                     (static_cast<std::uint32_t>(id[1]) << CHAR_BIT) | id[2];
        if (runtime_storage_diagnostics.flash_jedec_id != FLASH_JEDEC_ID) {
            runtime_storage_diagnostics.qspi_error = UNKNOWN_FLASH_ID_ERROR;
            return false;
        }

        UCHAR status_1{};
        UCHAR status_2{};
        if (!qspi_read_register(W25Q_READ_STATUS_1, status_1) ||
            !qspi_read_register(W25Q_READ_STATUS_2, status_2)) {
            return false;
        }
        if ((status_2 & 0x02U) == 0U) {
            std::array<UCHAR, 2U> statuses{ status_1, static_cast<UCHAR>(status_2 | 0x02U) };
            if (!qspi_write_enable() ||
                !qspi_command({ .instruction = W25Q_WRITE_STATUS,
                                .address = 0U,
                                .address_mode = QSPI_ADDRESS_NONE,
                                .data_mode = QSPI_DATA_1_LINE,
                                .count = statuses.size() }) ||
                HAL_QSPI_Transmit(&hqspi, statuses.data(), QSPI_TIMEOUT) != HAL_OK || !qspi_wait_ready() ||
                !qspi_read_register(W25Q_READ_STATUS_2, status_2) || (status_2 & 0x02U) == 0U) {
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
            if (HAL_GetTick() - started >= SD_TIMEOUT) {
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
            if (HAL_SD_ReadBlocks(&hsd1,
                                  sd_transfer_buffer.data(),
                                  static_cast<std::uint32_t>(sector + index),
                                  1U,
                                  SD_TIMEOUT) != HAL_OK) {
                runtime_storage_diagnostics.sd_error = hsd1.ErrorCode;
                return false;
            }
            if (!sd_wait_ready()) {
                return false;
            }
            std::memcpy(output + (index * SECTOR_SIZE), sd_transfer_buffer.data(), SECTOR_SIZE);
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
            std::memcpy(sd_transfer_buffer.data(), input + (index * SECTOR_SIZE), SECTOR_SIZE);
            if (HAL_SD_WriteBlocks(&hsd1,
                                   sd_transfer_buffer.data(),
                                   static_cast<std::uint32_t>(sector + index),
                                   1U,
                                   SD_TIMEOUT) != HAL_OK) {
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
                                        "/tmp/nucleo-h753zi-%ld-%s.img",
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
#if defined(HAL_PLATFORM_STM32)
        if (!initialize_qspi()) {
            return false;
        }
#if defined(RUNTIME_STORAGE_ERASE_FLASH_ON_BOOT)
        for (ULONG block{}; block < FLASH_BLOCKS; ++block) {
            if (!qspi_erase(static_cast<std::size_t>(block) * ERASE_BLOCK_BYTES)) {
                return false;
            }
        }
#endif
#else
        bool created{};
        if (!open_image({ .kind = "flash-levelx", .bytes = FLASH_BYTES, .fill = ERASED_FLASH_BYTE },
                        nor_file,
                        created)) {
            return false;
        }
        runtime_storage_diagnostics.flash_jedec_id = FLASH_JEDEC_ID;
#endif
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
#if defined(HAL_PLATFORM_STM32)
        runtime_storage_diagnostics.sd_detect_level =
          // CMSIS GPIO port addresses are fixed by the STM32 memory map.
          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
          HAL_GPIO_ReadPin(SD_CARD_DETECT_GPIO_Port, SD_CARD_DETECT_Pin) == GPIO_PIN_SET ? 1U : 0U;
        if (!initialize_sd_card(SdBusWidth::Four)) {
            const std::uint32_t four_bit_error{ hsd1.ErrorCode };
            const std::uint32_t four_bit_blocks{ hsd1.SdCard.LogBlockNbr };
            runtime_storage_diagnostics.sd_fallback_error = four_bit_error;
            runtime_storage_diagnostics.sd_fallback_used = 1U;
            static_cast<void>(HAL_SD_DeInit(&hsd1));
            if (!initialize_sd_card(SdBusWidth::One)) {
                if (runtime_storage_diagnostics.sd_blocks == 0U) {
                    runtime_storage_diagnostics.sd_blocks = four_bit_blocks;
                }
                return false;
            }
        }
#else
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
#endif
        UINT open_status{ fx_media_open(&sd_media,
                                        name.data(),
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
            if (!initialize_sd_card(SdBusWidth::One)) {
                if (runtime_storage_diagnostics.sd_blocks == 0U) {
                    runtime_storage_diagnostics.sd_blocks = four_bit_blocks;
                }
                runtime_storage_diagnostics.sd_filex_status = open_status;
                return false;
            }
            open_status = fx_media_open(&sd_media,
                                        name.data(),
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
        const bool flash_ok{ (hardware_storage_startup_test_volume_mask & 1U) != 0U &&
                             initialize_flash_media() };
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
        return volume == Volume::Flash ? &flash_media : &sd_media;
    }

    auto mounted(Volume volume) noexcept -> bool
    {
        return volume == Volume::Flash ? flash_mounted : sd_mounted;
    }

    auto diagnostics() noexcept -> const Diagnostics& { return runtime_storage_diagnostics; }
}
