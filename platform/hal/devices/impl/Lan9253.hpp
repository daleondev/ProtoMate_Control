#pragma once

#include "hal/drivers/itf/IDigitalOutput.hpp"
#include "hal/drivers/itf/ISpi.hpp"

namespace hal::device
{
    // LAN9255's ESC die, in ordinary SPI / LAN9252-compatible address mode
    // (PDI 0x80). System registers and the indirect EtherCAT CSR interface.
    // Direct-mapped PDI 0x82 and its 0x3000 register offset are NOT this mode.
    // One owner must serialize complete transactions on SPI and CS together.
    class Lan9253
    {
      public:
        static constexpr std::uint16_t ID_REVISION_ADDRESS = 0x0050;
        static constexpr std::uint16_t BYTE_TEST_ADDRESS = 0x0064;
        static constexpr std::uint16_t HARDWARE_CONFIG_ADDRESS = 0x0074;
        static constexpr std::uint32_t BYTE_TEST_VALUE = 0x87654321;
        static constexpr std::uint32_t READY_MASK = 1U << 27;
        static constexpr std::uint16_t CHIP_ID = 0x9253;

        static constexpr std::size_t CONFIGURATION_BYTES = 16;
        static constexpr std::chrono::milliseconds DEFAULT_TIMEOUT{ 100 };
        static constexpr std::chrono::milliseconds BOOT_TIMEOUT{ 1000 };

        struct Identity
        {
            std::uint32_t byte_test{}, hardware_config{}, id_revision{};
            constexpr bool operator==(const Identity&) const = default;
        };

        Lan9253(ISpi& spi, IDigitalOutput& chip_select) noexcept;
        [[nodiscard]] hal::util::Result<std::uint32_t> readSystemRegister(
          std::uint16_t address,
          std::chrono::milliseconds timeout = DEFAULT_TIMEOUT);
        [[nodiscard]] hal::util::Result<> writeSystemRegister(
          std::uint16_t address,
          std::uint32_t value,
          std::chrono::milliseconds timeout = DEFAULT_TIMEOUT);
        // Indirect accesses use aligned sizes 1, 2 or 4, within 0x0000..0x2fff.
        // Process RAM (0x1000..0x2fff) is also accessible through this interface
        // (DS00003421B 11.13.1); FIFO transfers are a future optimization.
        // Before READY, only use these for the EEPROM-emulation bootstrap
        // described by DS00003421B section 13.4, after BYTE_TEST succeeds.
        [[nodiscard]] hal::util::Result<std::uint32_t> readEscRegister(
          std::uint16_t address,
          std::uint8_t size,
          std::chrono::milliseconds timeout = DEFAULT_TIMEOUT);
        [[nodiscard]] hal::util::Result<> writeEscRegister(
          std::uint16_t address,
          std::uint8_t size,
          std::uint32_t value,
          std::chrono::milliseconds timeout = DEFAULT_TIMEOUT);
        // Serve only startup configuration requests (words 0 and 4). This is
        // volatile configuration, NOT EEPROM programming or a complete SII
        // emulator. Rejects non-emulation mode, writes and other addresses.
        // Requires PDI 0x80 and a valid CRC over the first 14 bytes.
        [[nodiscard]] hal::util::Result<Identity> initializeEmulatedBoot(
          std::span<const std::uint8_t, CONFIGURATION_BYTES> configuration,
          std::chrono::milliseconds timeout = BOOT_TIMEOUT);
        // Single attempt: BYTE_TEST, then READY, then ID. Never reads ID while
        // not ready. The caller may retry within its own startup deadline.
        [[nodiscard]] hal::util::Result<Identity> identify(
          std::chrono::milliseconds timeout = DEFAULT_TIMEOUT);

      private:
        [[nodiscard]] hal::util::Result<> serviceEmulatedRequest(
          std::span<const std::uint8_t, CONFIGURATION_BYTES> configuration,
          std::chrono::steady_clock::time_point deadline);
        [[nodiscard]] hal::util::Result<> waitCsr(std::chrono::steady_clock::time_point deadline);
        ISpi& m_spi; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members) -- Required, non-owning
                     // dependency; its owner outlives this object.
        IDigitalOutput& m_chipSelect; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members) --
                                      // Required, non-owning dependency; its owner outlives this object.
    };
}
