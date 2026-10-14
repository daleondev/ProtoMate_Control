#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace runtime::storage
{
    // Raw, partition-relative access. Each 32-byte word may be programmed only
    // once between erases, including words whose visible bytes remain all ones.
    class FlashDevice
    {
      public:
        virtual ~FlashDevice() = default;
        virtual bool read(std::size_t offset, std::span<std::byte> output) noexcept = 0;
        virtual bool program(std::size_t offset, std::span<const std::byte, 32> word) noexcept = 0;
        virtual bool erase(std::size_t block) noexcept = 0;
    };

    // FileX sector translation for ECC flash. No in-place metadata updates.
    // A data record is committed by its final flashword. GC copies every live
    // record before erasing its source; scanning reconstructs interrupted GC.
    // Caller serializes access (the FileX media mutex in production).
    class FlashJournal
    {
      public:
        static constexpr std::size_t BLOCK_BYTES{ 128U * 1024U };
        static constexpr std::size_t BLOCKS{ 4 };
        static constexpr std::size_t BYTES{ BLOCK_BYTES * BLOCKS };
        static constexpr std::size_t SECTOR_BYTES{ 512 };
        static constexpr std::size_t SECTORS{ 512 }; // 256 KiB logical, 512 KiB physical.
        static constexpr std::size_t RECORD_BYTES{ SECTOR_BYTES + 32 };
        static constexpr std::size_t RECORDS_PER_BLOCK{ BLOCK_BYTES / RECORD_BYTES };
        static constexpr std::size_t RECORDS{ RECORDS_PER_BLOCK * BLOCKS };
        enum class Mount { Ready, Blank, Corrupt };

        explicit FlashJournal(FlashDevice& device) : m_device{ device } {}
        Mount mount() noexcept;
        bool read(std::size_t sector, std::span<std::byte, SECTOR_BYTES> data) noexcept;
        bool write(std::size_t sector, std::span<const std::byte, SECTOR_BYTES> data) noexcept;
        // FileX can reclaim sectors on deletion; a tombstone must itself survive reset.
        bool release(std::size_t sector) noexcept;
        std::uint32_t erases() const noexcept { return m_erases; }
        static constexpr std::size_t offset(std::size_t slot) noexcept
        {
            return (slot / RECORDS_PER_BLOCK) * BLOCK_BYTES +
                   (slot % RECORDS_PER_BLOCK) * RECORD_BYTES;
        }

      private:
        static constexpr std::uint16_t NONE{ 0xFFFF };
        struct Entry
        {
            std::uint64_t sequence{};
            std::uint32_t crc{};
            std::uint16_t slot{ NONE };
            bool deleted{};
        };
        bool append(std::size_t sector, std::span<const std::byte, SECTOR_BYTES> data,
                    bool deleted, std::size_t slot) noexcept;
        bool reclaim() noexcept;
        std::size_t freeSlot(std::size_t excludedBlock = BLOCKS) const noexcept;
        bool isEmpty(std::size_t block) const noexcept;
        FlashDevice& m_device;
        std::array<Entry, SECTORS> m_map{};
        std::array<bool, RECORDS> m_free{};
        std::uint64_t m_sequence{};
        std::size_t m_cursor{};
        std::uint32_t m_erases{};
        bool m_ready{};
    };
}
