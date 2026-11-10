// CMSIS/HAL register macros use fixed MMIO addresses and C pointer casts.
// NOLINTBEGIN(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
#include "InternalFlash.hpp"
#include "boot/giga/InternalFlashLayout.h"
#include "stm32h7xx_hal.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace runtime::storage
{
    namespace
    {
        constexpr std::byte ERASED_BYTE{ 0xFF };
    }
    static_assert(FlashJournal::BYTES == STORAGE_INTERNAL_BYTES);
    static_assert(FlashJournal::BLOCK_BYTES == STORAGE_INTERNAL_ERASE_BYTES);

    bool InternalFlash::read(std::size_t offset, std::span<std::byte> output) noexcept
    {
        if (offset > STORAGE_INTERNAL_BYTES || output.size() > STORAGE_INTERNAL_BYTES - offset) {
            return false;
        }
        while (!output.empty()) {
            const auto address{ STORAGE_INTERNAL_ORIGIN + (offset & ~(WORD_BYTES - 1)) };
            const auto inside{ offset & (WORD_BYTES - 1) };
            const auto count{ std::min(output.size(), WORD_BYTES - inside) };
            std::array<std::uint32_t, WORD_BYTES / sizeof(std::uint32_t)> word{};
            // A reset during programming can leave invalid ECC. Probe a single
            // noncacheable flashword at elevated fault priority, ignoring only
            // synchronous data BusFaults during these loads. RM0399 4.7.7;
            // Cortex-M7 BFHFNMIGN + FAULTMASK. No ISR or scheduler runs here.
            const auto previous_mask{ __get_FAULTMASK() };
            // CMSIS peripheral access uses the MCU fixed-address register map.
            // NOLINTNEXTLINE(clang-analyzer-core.FixedAddressDereference)
            const auto previous_ccr{ SCB->CCR };
            __disable_fault_irq();
            SCB->CCR = previous_ccr | SCB_CCR_BFHFNMIGN_Msk;
            // CMSIS peripheral access uses the MCU fixed-address register map.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            FLASH->CCR2 = FLASH_CCR_CLR_DBECCERR | FLASH_CCR_CLR_SNECCERR;
            __DSB();
            __ISB();
            const auto* source{ reinterpret_cast<volatile const std::uint32_t*>(address) };
            for (std::size_t i{}; i < word.size(); ++i) {
                word[i] = source[i];
            }
            __DSB();
            // CMSIS peripheral access uses the MCU fixed-address register map.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            const auto errors{ FLASH->SR2 & (FLASH_SR_DBECCERR | FLASH_SR_SNECCERR) };
            // CMSIS peripheral access uses the MCU fixed-address register map.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
            FLASH->CCR2 = FLASH_CCR_CLR_DBECCERR | FLASH_CCR_CLR_SNECCERR;
            SCB->CCR = previous_ccr;
            __DSB();
            __ISB();
            __set_FAULTMASK(previous_mask);
            if ((errors & FLASH_SR_DBECCERR) != 0U) {
                m_error = errors;
                return false;
            }
            std::memcpy(output.data(), std::as_bytes(std::span(word)).data() + inside, count);
            output = output.subspan(count);
            offset += count;
        }
        return true;
    }

    bool InternalFlash::program(std::size_t offset, std::span<const std::byte, WORD_BYTES> word) noexcept
    {
        if ((offset % WORD_BYTES) != 0 || offset > STORAGE_INTERNAL_BYTES - WORD_BYTES) {
            return false;
        }
        alignas(WORD_BYTES) std::array<std::byte, WORD_BYTES> bytes{};
        if (!read(offset, bytes) ||
            !std::ranges::all_of(bytes, [](std::byte b) { return b == ERASED_BYTE; })) {
            return false;
        }
        if (std::ranges::all_of(word, [](std::byte b) { return b == ERASED_BYTE; })) {
            return true;
        }
        std::ranges::copy(word, bytes.begin());
        if (HAL_FLASH_Unlock() != HAL_OK) {
            m_error = HAL_FLASH_GetError();
            return false;
        }
        __HAL_FLASH_CLEAR_FLAG_BANK2(FLASH_FLAG_ALL_ERRORS_BANK2);
        const auto status{ HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD,
                                             STORAGE_INTERNAL_ORIGIN + static_cast<std::uint32_t>(offset),
                                             reinterpret_cast<std::uint32_t>(bytes.data())) };
        m_error = HAL_FLASH_GetError();
        const auto locked{ HAL_FLASH_Lock() };
        __DSB();
        return status == HAL_OK && locked == HAL_OK && read(offset, bytes) && std::ranges::equal(bytes, word);
    }

    bool InternalFlash::erase(std::size_t block) noexcept
    {
        if (block >= FlashJournal::BLOCKS) {
            return false;
        }
        FLASH_EraseInitTypeDef request{};
        request.TypeErase = FLASH_TYPEERASE_SECTORS;
        request.Banks = FLASH_BANK_2;
        request.Sector = STORAGE_INTERNAL_FIRST_SECTOR + static_cast<std::uint32_t>(block);
        request.NbSectors = 1;
        request.VoltageRange = FLASH_VOLTAGE_RANGE_3;
        std::uint32_t failed{};
        if (HAL_FLASH_Unlock() != HAL_OK) {
            m_error = HAL_FLASH_GetError();
            return false;
        }
        __HAL_FLASH_CLEAR_FLAG_BANK2(FLASH_FLAG_ALL_ERRORS_BANK2);
        const auto status{ HAL_FLASHEx_Erase(&request, &failed) };
        m_error = HAL_FLASH_GetError();
        const auto locked{ HAL_FLASH_Lock() };
        __DSB();
        if (status != HAL_OK || locked != HAL_OK) {
            return false;
        }
        std::array<std::byte, WORD_BYTES> bytes{};
        for (std::size_t at{}; at < FlashJournal::BLOCK_BYTES; at += bytes.size()) {
            if (!read((block * FlashJournal::BLOCK_BYTES) + at, bytes) ||
                !std::ranges::all_of(bytes, [](std::byte b) { return b == ERASED_BYTE; })) {
                return false;
            }
        }
        return true;
    }
}

// NOLINTEND(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
