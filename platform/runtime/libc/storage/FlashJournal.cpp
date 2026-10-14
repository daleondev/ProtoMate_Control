#include "FlashJournal.hpp"

#include <algorithm>
#include <limits>

namespace runtime::storage
{
    namespace
    {
        constexpr std::uint32_t MAGIC{ 0x314A4D50 }; // PMJ1, little endian.
        constexpr std::uint32_t VERSION{ 1 };
        std::uint32_t crc(std::span<const std::byte> data) noexcept
        {
            std::uint32_t value{ 0xFFFFFFFFU };
            for (const auto byte : data) {
                value ^= std::to_integer<std::uint32_t>(byte);
                for (unsigned bit{}; bit < 8; ++bit) {
                    value = (value >> 1) ^ (0xEDB88320U & (0U - (value & 1U)));
                }
            }
            return ~value;
        }
        std::uint32_t get(std::span<const std::byte> data, std::size_t at) noexcept
        {
            std::uint32_t value{};
            for (unsigned i{}; i < 4; ++i) { value |= std::to_integer<std::uint32_t>(data[at + i]) << (i * 8); }
            return value;
        }
        void put(std::span<std::byte> data, std::size_t at, std::uint32_t value) noexcept
        {
            for (unsigned i{}; i < 4; ++i) { data[at + i] = std::byte(value >> (i * 8)); }
        }
        bool erased(std::span<const std::byte> bytes) noexcept
        {
            return std::ranges::all_of(bytes, [](std::byte value) { return value == std::byte{ 0xFF }; });
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
            const auto address = offset(slot);
            auto header = std::span{ record }.last<32>();
            const bool headerRead = m_device.read(address + SECTOR_BYTES, header);
            const bool dataRead = m_device.read(address, std::span{ record }.first<SECTOR_BYTES>());
            m_free[slot] = headerRead && dataRead && erased(record);
            blank = blank && m_free[slot];
            if (m_free[slot] || !headerRead || get(header, 0) != MAGIC ||
                get(header, 4) != VERSION || crc(header.first<28>()) != get(header, 28)) { continue; }
            const auto sector = get(header, 16);
            const auto deleted = get(header, 20);
            const auto sequence = std::uint64_t{ get(header, 8) } | (std::uint64_t{ get(header, 12) } << 32);
            if (sector >= SECTORS || deleted > 1 || sequence == 0) { return Mount::Corrupt; }
            if (sequence > m_sequence) { m_sequence = sequence; m_cursor = (slot / RECORDS_PER_BLOCK); }
            const auto checksum = get(header, 24);
            if (!dataRead || crc(std::span{ record }.first<SECTOR_BYTES>()) != checksum) {
                damaged[sector] = std::max(damaged[sector], sequence);
                continue;
            }
            valid = true;
            if (sequence > m_map[sector].sequence) {
                m_map[sector] = { sequence, checksum, static_cast<std::uint16_t>(slot), deleted != 0 };
            }
        }
        // Unused tail bytes must also be erased before automatic first formatting.
        for (std::size_t block{}; block < BLOCKS && blank; ++block) {
            const auto tail = block * BLOCK_BYTES + RECORDS_PER_BLOCK * RECORD_BYTES;
            auto bytes = std::span{ record }.first(BLOCK_BYTES - RECORDS_PER_BLOCK * RECORD_BYTES);
            blank = m_device.read(tail, bytes) && erased(bytes);
        }
        for (std::size_t sector{}; sector < SECTORS; ++sector) {
            if (damaged[sector] > m_map[sector].sequence) { return Mount::Corrupt; }
        }
        if (!blank && !valid) { return Mount::Corrupt; }
        m_ready = true;
        return blank ? Mount::Blank : Mount::Ready;
    }

    bool FlashJournal::read(std::size_t sector, std::span<std::byte, SECTOR_BYTES> data) noexcept
    {
        if (!m_ready || sector >= SECTORS) { return false; }
        const auto& entry = m_map[sector];
        if (entry.slot == NONE || entry.deleted) { std::ranges::fill(data, std::byte{}); return true; }
        return m_device.read(offset(entry.slot), data) && crc(data) == entry.crc;
    }

    bool FlashJournal::isEmpty(std::size_t block) const noexcept
    {
        const auto first = m_free.begin() + static_cast<std::ptrdiff_t>(block * RECORDS_PER_BLOCK);
        return std::all_of(first, first + RECORDS_PER_BLOCK, [](bool free) { return free; });
    }

    std::size_t FlashJournal::freeSlot(std::size_t excludedBlock) const noexcept
    {
        for (std::size_t i{}; i < RECORDS; ++i) {
            const auto slot = (m_cursor * RECORDS_PER_BLOCK + i) % RECORDS;
            if (slot / RECORDS_PER_BLOCK != excludedBlock && m_free[slot]) { return slot; }
        }
        return RECORDS;
    }

    bool FlashJournal::append(std::size_t sector, std::span<const std::byte, SECTOR_BYTES> data,
                              bool deleted, std::size_t slot) noexcept
    {
        if (m_sequence == std::numeric_limits<std::uint64_t>::max() || slot >= RECORDS || !m_free[slot]) {
            return false;
        }
        m_free[slot] = false; // A failed/torn program must never be retried in place.
        const auto sequence = ++m_sequence;
        std::array<std::byte, 32> header{};
        put(header, 0, MAGIC); put(header, 4, VERSION);
        put(header, 8, static_cast<std::uint32_t>(sequence)); put(header, 12, static_cast<std::uint32_t>(sequence >> 32));
        put(header, 16, static_cast<std::uint32_t>(sector)); put(header, 20, deleted ? 1U : 0U);
        const auto checksum = crc(data);
        put(header, 24, checksum); put(header, 28, crc(std::span{ header }.first<28>()));
        for (std::size_t at{}; at < SECTOR_BYTES; at += 32) {
            auto word = std::span<const std::byte, 32>{ data.data() + at, 32 };
            // Leave all-ones flashwords physically erased, including their ECC bits.
            if (!erased(word) && !m_device.program(offset(slot) + at, word)) { m_ready = false; return false; }
        }
        if (!m_device.program(offset(slot) + SECTOR_BYTES, header)) { m_ready = false; return false; }
        m_map[sector] = { sequence, checksum, static_cast<std::uint16_t>(slot), deleted };
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
            const auto block = (m_cursor + i) % BLOCKS;
            if (isEmpty(block)) { continue; }
            const auto live = static_cast<std::size_t>(std::count_if(m_map.begin(), m_map.end(), [block](const Entry& e) {
                return e.slot != NONE && e.slot / RECORDS_PER_BLOCK == block;
            }));
            std::size_t outside{};
            for (std::size_t slot{}; slot < RECORDS; ++slot) {
                if (slot / RECORDS_PER_BLOCK != block && m_free[slot]) { ++outside; }
            }
            if (live <= outside && live < least) { victim = block; least = live; }
        }
        if (victim == BLOCKS) { return false; }
        std::array<std::byte, SECTOR_BYTES> data{};
        for (std::size_t sector{}; sector < SECTORS; ++sector) {
            const auto entry = m_map[sector];
            if (entry.slot == NONE || entry.slot / RECORDS_PER_BLOCK != victim) { continue; }
            if (!m_device.read(offset(entry.slot), data) || crc(data) != entry.crc ||
                !append(sector, data, entry.deleted, freeSlot(victim))) { return false; }
        }
        if (!m_device.erase(victim)) { m_ready = false; return false; }
        ++m_erases;
        auto first = m_free.begin() + static_cast<std::ptrdiff_t>(victim * RECORDS_PER_BLOCK);
        std::fill_n(first, RECORDS_PER_BLOCK, true);
        m_cursor = (victim + 1) % BLOCKS;
        return true;
    }

    bool FlashJournal::write(std::size_t sector, std::span<const std::byte, SECTOR_BYTES> data) noexcept
    {
        if (!m_ready || sector >= SECTORS) { return false; }
        std::array<std::byte, SECTOR_BYTES> current{};
        if (!read(sector, current)) { return false; }
        if (std::ranges::equal(current, data)) { return true; }
        for (std::size_t attempt{}; attempt <= BLOCKS; ++attempt) {
            std::size_t spare{ BLOCKS };
            for (std::size_t i{}; i < BLOCKS; ++i) { if (isEmpty(i)) { spare = i; break; } }
            if (spare != BLOCKS && freeSlot(spare) != RECORDS) {
                return append(sector, data, false, freeSlot(spare));
            }
            // Removing an interrupted copy may only restore the spare block;
            // a second collection then creates room for the new record.
            if (!reclaim()) { return false; }
        }
        return false;
    }

    bool FlashJournal::release(std::size_t sector) noexcept
    {
        if (!m_ready || sector >= SECTORS) { return false; }
        if (m_map[sector].slot == NONE || m_map[sector].deleted) { return true; }
        // Zero sectors are valid logical contents after release. Keeping a
        // committed zero record prevents old data from resurfacing after reset.
        std::array<std::byte, SECTOR_BYTES> zero{};
        return write(sector, zero);
    }
}
