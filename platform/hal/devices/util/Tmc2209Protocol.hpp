#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace hal::device::util::tmc2209
{
    inline constexpr std::size_t BYTE_BITS{ 8 };
    inline constexpr std::size_t REGISTER_BYTES{ 4 };
    inline constexpr std::size_t DATA_OFFSET{ 3 };
    inline constexpr std::size_t READ_REQUEST_BYTES{ 4 };
    inline constexpr std::size_t FRAME_BYTES{ 8 };
    inline constexpr std::size_t REGISTER_COUNT{ 128 };
    inline constexpr std::uint8_t SYNC{ 0x05 };
    inline constexpr std::uint8_t MASTER_ADDRESS{ 0xFF };
    inline constexpr std::uint8_t CRC_POLYNOMIAL{ 0x07 };
    inline constexpr std::uint8_t WRITE_FLAG{ 0x80 };
    inline constexpr std::uint8_t REGISTER_MASK{ 0x7F };
    inline constexpr auto TRANSACTION_TIMEOUT{ std::chrono::milliseconds{ 10 } };

    inline constexpr std::uint8_t GCONF{ 0x00 }, GSTAT{ 0x01 }, IFCNT{ 0x02 }, NODECONF{ 0x03 };
    inline constexpr std::uint8_t IOIN{ 0x06 }, SG_RESULT{ 0x41 }, DRV_STATUS{ 0x6F };
    inline constexpr std::uint8_t CHOPCONF{ 0x6C }, PWMCONF{ 0x70 };
    inline constexpr std::uint32_t IC_VERSION{ 0x21 }, VERSION_SHIFT{ 24 };
    inline constexpr std::uint32_t IOIN_SPREAD{ 1U << 8U };
    inline constexpr std::uint32_t GCONF_PDN_DISABLE{ 1U << 6U };
    inline constexpr std::uint32_t GCONF_MSTEP_REG_SELECT{ 1U << 7U };
    inline constexpr std::uint32_t GCONF_MULTISTEP_FILTER{ 1U << 8U };
    inline constexpr std::uint32_t GCONF_INDEX_STEP{ 1U << 5U };
    inline constexpr std::uint32_t GCONF_SPREADCYCLE{ 1U << 2U };
    inline constexpr std::uint32_t CHOPCONF_BASE{ 0x00020053U };
    inline constexpr std::uint32_t CHOPCONF_INTERPOLATE{ 1U << 28U };
    inline constexpr std::uint32_t MRES_SHIFT{ 24 }, MRES_EXPONENT{ 8 };
    inline constexpr std::uint32_t GCONF_RESET{ 0x101 }, IOIN_RESET{ 0x21000001 };
    inline constexpr std::uint32_t CHOPCONF_RESET{ 0x10000053 }, DRV_STATUS_RESET{ 0x80000000 };
    inline constexpr std::uint32_t PWMCONF_DEFAULT{ 0xC10D0024 }, LOAD_MASK{ 0x3FF };
    inline constexpr std::uint16_t MIN_CURRENT_MA{ 100 }, MAX_CURRENT_MA{ 1800 };
    inline constexpr double CURRENT_SCALE_STEPS{ 32.0 };
    inline constexpr std::uint16_t MAX_MICROSTEPS{ 256 };
    inline constexpr std::uint8_t MIN_STEALTH_CURRENT_SCALE{ 8 };

    [[nodiscard]] constexpr auto read_word(std::span<const std::uint8_t, REGISTER_BYTES> bytes) noexcept
      -> std::uint32_t
    {
        std::uint32_t value{};
        for (const auto byte : bytes)
            value = (value << BYTE_BITS) | byte;
        return value;
    }

    constexpr auto write_word(std::span<std::uint8_t, REGISTER_BYTES> bytes, std::uint32_t value) noexcept
      -> void
    {
        for (std::size_t index{}; index < bytes.size(); ++index)
            bytes[index] = static_cast<std::uint8_t>(value >> ((REGISTER_BYTES - 1U - index) * BYTE_BITS));
    }
}
