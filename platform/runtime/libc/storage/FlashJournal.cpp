#include "FlashJournal.hpp"

#include <algorithm>
#include <limits>

namespace runtime::storage
{
    namespace
    {
        constexpr std::uint32_t MAGIC{ 0x314A4D50 }; // PMJ1, little endian.
        constexpr std::uint32_t VERSION{ 1 }, CRC_INITIAL{ 0xFFFFFFFFU }, CRC_POLYNOMIAL{ 0xEDB88320U };
        constexpr unsigned BYTE_BITS{ 8 }, WORD_BITS{ 32 };
        constexpr std::byte ERASED_BYTE{ 0xFF };
        constexpr std::size_t HEADER_BYTES{ 32 }, FLASH_WORD_BYTES{ 32 }, VERSION_OFFSET{ 4 },
          SEQUENCE_LOW_OFFSET{ 8 }, SEQUENCE_HIGH_OFFSET{ 12 }, SECTOR_OFFSET{ 16 }, DELETED_OFFSET{ 20 },
          PAYLOAD_CRC_OFFSET{ 24 }, HEADER_CRC_OFFSET{ 28 };
        std::uint32_t crc(std::span<const std::byte> data) noexcept
        {
            std::uint32_t value{ CRC_INITIAL };
            for (const auto byte : data) {
                value ^= std::to_integer<std::uint32_t>(byte);
                for (unsigned bit{}; bit < BYTE_BITS; ++bit) {
                    value = (value >> 1) ^ (CRC_POLYNOMIAL & (0U - (value & 1U)));
                }
            }
            return ~value;
        }
        std::uint32_t get(std::span<const std::byte> data, std::size_t at) noexcept
        {
            std::uint32_t value{};
            for (unsigned i{}; i < 4; ++i) {
                value |= std::to_integer<std::uint32_t>(data[at + i]) << (i * BYTE_BITS);
            }
            return value;
        }
        void put(std::span<std::byte> data, std::size_t at, std::uint32_t value) noexcept
        {
            for (unsigned i{}; i < 4; ++i) {
                data[at + i] = std::byte(value >> (i * BYTE_BITS));
            }
        }
        bool erased(std::span<const std::byte> bytes) noexcept
        {
            return std::ranges::all_of(bytes, [](std::byte value) { return value == ERASED_BYTE; });
        }
    }

    FlashJournal::Mount FlashJournal::mount() noexcept
    {
        m_ready = false;
        m_map = {};
        m_free = {};
        m_sequence = 0;
        m_cursor = 0;
        // Only needed while mounting; no allocation and bounded startup stack use.
        std::array<std::uint64_t, SECTORS> damaged{};
        bool blank{ true };
        bool valid{};
        std::array<std::byte, RECORD_BYTES> record{};
        for (std::size_t slot{}; slot < RECORDS; ++slot) {
            const auto address{ offset(slot) };
            auto header{ std::span{ record }.last<HEADER_BYTES>() };
            const bool header_read{ m_device.read(address + SECTOR_BYTES, header) };
            const bool data_read{ m_device.read(address, std::span{ record }.first<SECTOR_BYTES>()) };
            m_free[slot] = header_read && data_read && erased(record);
            blank = blank && m_free[slot];
            if (m_free[slot] || !header_read || get(header, 0) != MAGIC ||
                get(header, VERSION_OFFSET) != VERSION ||
                crc(header.first<HEADER_CRC_OFFSET>()) != get(header, HEADER_CRC_OFFSET)) {
                continue;
            }
            const auto sector{ get(header, SECTOR_OFFSET) };
            const auto deleted{ get(header, DELETED_OFFSET) };
            const auto sequence{ std::uint64_t{ get(header, SEQUENCE_LOW_OFFSET) } |
                                 (std::uint64_t{ get(header, SEQUENCE_HIGH_OFFSET) } << WORD_BITS) };
            if (sector >= SECTORS || deleted > 1 || sequence == 0) {
                return Mount::Corrupt;
            }
            if (sequence > m_sequence) {
                m_sequence = sequence;
                m_cursor = (slot / RECORDS_PER_BLOCK);
            }
            const auto checksum{ get(header, PAYLOAD_CRC_OFFSET) };
            if (!data_read || crc(std::span{ record }.first<SECTOR_BYTES>()) != checksum) {
                damaged[sector] = std::max(damaged[sector], sequence);
                continue;
            }
            valid = true;
            if (sequence > m_map[sector].sequence) {
                m_map[sector] = { .sequence = sequence,
                                  .crc = checksum,
                                  .slot = static_cast<std::uint16_t>(slot),
                                  .deleted = deleted != 0 };
            }
        }
        // Unused tail bytes must also be erased before automatic first formatting.
        for (std::size_t block{}; block < BLOCKS && blank; ++block) {
            const auto tail{ (block * BLOCK_BYTES) + (RECORDS_PER_BLOCK * RECORD_BYTES) };
            auto bytes{ std::span{ record }.first(BLOCK_BYTES - (RECORDS_PER_BLOCK * RECORD_BYTES)) };
            blank = m_device.read(tail, bytes) && erased(bytes);
        }
        for (std::size_t sector{}; sector < SECTORS; ++sector) {
            if (damaged[sector] > m_map[sector].sequence) {
                return Mount::Corrupt;
            }
        }
        if (!blank && !valid) {
            return Mount::Corrupt;
        }
        m_ready = true;
        return blank ? Mount::Blank : Mount::Ready;
    }

    bool FlashJournal::read(std::size_t sector, std::span<std::byte, SECTOR_BYTES> data) noexcept
    {
        if (!m_ready || sector >= SECTORS) {
            return false;
        }
        const auto& entry{ m_map[sector] };
        if (entry.slot == NONE || entry.deleted) {
            std::ranges::fill(data, std::byte{});
            return true;
        }
        return m_device.read(offset(entry.slot), data) && crc(data) == entry.crc;
    }

    bool FlashJournal::isEmpty(std::size_t block) const noexcept
    {
        const auto* const first{ m_free.begin() + static_cast<std::ptrdiff_t>(block * RECORDS_PER_BLOCK) };
        return std::all_of(first, first + RECORDS_PER_BLOCK, [](bool free) { return free; });
    }

    std::size_t FlashJournal::freeSlot(std::size_t excluded_block) const noexcept
    {
        for (std::size_t i{}; i < RECORDS; ++i) {
            const auto slot{ (m_cursor * RECORDS_PER_BLOCK + i) % RECORDS };
            if (slot / RECORDS_PER_BLOCK != excluded_block && m_free[slot]) {
                return slot;
            }
        }
        return RECORDS;
    }

    bool FlashJournal::append(std::size_t sector,
                              std::span<const std::byte, SECTOR_BYTES> data,
                              bool deleted,
                              std::size_t slot) noexcept
    {
        if (m_sequence == std::numeric_limits<std::uint64_t>::max() || slot >= RECORDS || !m_free[slot]) {
            return false;
        }
        m_free[slot] = false; // A failed/torn program must never be retried in place.
        const auto sequence{ ++m_sequence };
        std::array<std::byte, HEADER_BYTES> header{};
        put(header, 0, MAGIC);
        put(header, VERSION_OFFSET, VERSION);
        put(header, SEQUENCE_LOW_OFFSET, static_cast<std::uint32_t>(sequence));
        put(header, SEQUENCE_HIGH_OFFSET, static_cast<std::uint32_t>(sequence >> WORD_BITS));
        put(header, SECTOR_OFFSET, static_cast<std::uint32_t>(sector));
        put(header, DELETED_OFFSET, deleted ? 1U : 0U);
        const auto checksum{ crc(data) };
        put(header, PAYLOAD_CRC_OFFSET, checksum);
        put(header, HEADER_CRC_OFFSET, crc(std::span{ header }.first<HEADER_CRC_OFFSET>()));
        for (std::size_t at{}; at < SECTOR_BYTES; at += FLASH_WORD_BYTES) {
            auto word{ std::span<const std::byte, FLASH_WORD_BYTES>{ data.data() + at, FLASH_WORD_BYTES } };
            // Leave all-ones flashwords physically erased, including their ECC bits.
            if (!erased(word) && !m_device.program(offset(slot) + at, word)) {
                m_ready = false;
                return false;
            }
        }
        if (!m_device.program(offset(slot) + SECTOR_BYTES, header)) {
            m_ready = false;
            return false;
        }
        m_map[sector] = { .sequence = sequence,
                          .crc = checksum,
                          .slot = static_cast<std::uint16_t>(slot),
                          .deleted = deleted };
        return true;
    }

    bool FlashJournal::reclaim() noexcept
    {
        // With <=512 live records and 960 slots, one full spare erase block is
        // sufficient. After an interrupted collection its remaining copies fit
        // outside the same source block, even though no block is entirely empty.
        std::size_t victim{ BLOCKS };
        std::size_t least{ SECTORS + 1 };
        for (std::size_t i{}; i < BLOCKS; ++i) {
            const auto block{ (m_cursor + i) % BLOCKS };
            if (isEmpty(block)) {
                continue;
            }
            const auto live{ static_cast<std::size_t>(
              std::count_if(m_map.begin(), m_map.end(), [block](const Entry& e) {
                return e.slot != NONE && e.slot / RECORDS_PER_BLOCK == block;
            })) };
            std::size_t outside{};
            for (std::size_t slot{}; slot < RECORDS; ++slot) {
                if (slot / RECORDS_PER_BLOCK != block && m_free[slot]) {
                    ++outside;
                }
            }
            if (live <= outside && live < least) {
                victim = block;
                least = live;
            }
        }
        if (victim == BLOCKS) {
            return false;
        }
        std::array<std::byte, SECTOR_BYTES> data{};
        for (std::size_t sector{}; sector < SECTORS; ++sector) {
            const auto entry{ m_map[sector] };
            if (entry.slot == NONE || entry.slot / RECORDS_PER_BLOCK != victim) {
                continue;
            }
            if (!m_device.read(offset(entry.slot), data) || crc(data) != entry.crc ||
                !append(sector, data, entry.deleted, freeSlot(victim))) {
                return false;
            }
        }
        if (!m_device.erase(victim)) {
            m_ready = false;
            return false;
        }
        ++m_erases;
        auto* first{ m_free.begin() + static_cast<std::ptrdiff_t>(victim * RECORDS_PER_BLOCK) };
        std::fill_n(first, RECORDS_PER_BLOCK, true);
        m_cursor = (victim + 1) % BLOCKS;
        return true;
    }

    bool FlashJournal::write(std::size_t sector, std::span<const std::byte, SECTOR_BYTES> data) noexcept
    {
        if (!m_ready || sector >= SECTORS) {
            return false;
        }
        std::array<std::byte, SECTOR_BYTES> current{};
        if (!read(sector, current)) {
            return false;
        }
        if (std::ranges::equal(current, data)) {
            return true;
        }
        for (std::size_t attempt{}; attempt <= BLOCKS; ++attempt) {
            std::size_t spare{ BLOCKS };
            for (std::size_t i{}; i < BLOCKS; ++i) {
                if (isEmpty(i)) {
                    spare = i;
                    break;
                }
            }
            if (spare != BLOCKS && freeSlot(spare) != RECORDS) {
                return append(sector, data, false, freeSlot(spare));
            }
            // Removing an interrupted copy may only restore the spare block;
            // a second collection then creates room for the new record.
            if (!reclaim()) {
                return false;
            }
        }
        return false;
    }

    bool FlashJournal::release(std::size_t sector) noexcept
    {
        if (!m_ready || sector >= SECTORS) {
            return false;
        }
        if (m_map[sector].slot == NONE || m_map[sector].deleted) {
            return true;
        }
        // Zero sectors are valid logical contents after release. Keeping a
        // committed zero record prevents old data from resurfacing after reset.
        std::array<std::byte, SECTOR_BYTES> zero{};
        return write(sector, zero);
    }
}
