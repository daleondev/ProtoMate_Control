#include "WifiProfile.hpp"
#include <algorithm>

namespace ui::opcua::util
{
    namespace
    {
        constexpr std::size_t CRC_OFFSET{ 109 }, SSID_OFFSET{ 12 }, PASSWORD_OFFSET{ 45 };
        std::uint32_t checksum(std::span<const unsigned char> data) noexcept
        {
            std::uint32_t crc{ 0xFFFFFFFFU };
            for (const auto value : data) {
                crc ^= value;
                for (unsigned i{}; i < 8; ++i) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1)));
            }
            return ~crc;
        }
        bool valid_string(std::span<const unsigned char> data, std::size_t minimum) noexcept
        {
            auto end{ std::ranges::find(data, 0) };
            if (end == data.end() || static_cast<std::size_t>(end - data.begin()) < minimum) return false;
            if (std::any_of(data.begin(), end, [](auto c) { return c < 32 || c == 127; })) return false;
            return std::all_of(end, data.end(), [](auto c) { return c == 0; });
        }
    }
    void erase_credentials(void* memory, std::size_t size) noexcept
    {
        auto* out{ static_cast<volatile unsigned char*>(memory) };
        while (size--) *out++ = 0;
    }
    WifiProfile::Record WifiProfile::encode() const noexcept
    {
        Record record{};
        record[0] = 2;
        for (unsigned i{}; i < 8; ++i) record[4 + i] = static_cast<unsigned char>(generation >> (i * 8));
        std::copy(credentials.ssid.begin(), credentials.ssid.end(), record.begin() + SSID_OFFSET);
        std::copy(credentials.password.begin(), credentials.password.end(), record.begin() + PASSWORD_OFFSET);
        const auto crc{ checksum(std::span{ record }.first(CRC_OFFSET)) };
        for (unsigned i{}; i < 4; ++i) record[CRC_OFFSET + i] = static_cast<unsigned char>(crc >> (i * 8));
        return record;
    }
    bool WifiProfile::decode(std::span<const unsigned char> record, WifiProfile& output) noexcept
    {
        if (record.size() != Record{}.size() || record[0] != 2 || record[1] || record[2] || record[3]) return false;
        std::uint32_t crc{};
        for (unsigned i{}; i < 4; ++i) crc |= static_cast<std::uint32_t>(record[CRC_OFFSET + i]) << (i * 8);
        if (crc != checksum(record.first(CRC_OFFSET)) || !valid_string(record.subspan(SSID_OFFSET, 33), 1) ||
            !valid_string(record.subspan(PASSWORD_OFFSET, 64), 8)) return false;
        std::uint64_t generation{};
        for (unsigned i{}; i < 8; ++i) generation |= static_cast<std::uint64_t>(record[4 + i]) << (i * 8);
        if (!generation) return false;
        output.generation = generation;
        std::copy_n(record.begin() + SSID_OFFSET, 33, output.credentials.ssid.begin());
        std::copy_n(record.begin() + PASSWORD_OFFSET, 64, output.credentials.password.begin());
        return true;
    }
}
