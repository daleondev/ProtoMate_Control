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
        static constexpr std::uint16_t idRevisionAddress = 0x0050;
        static constexpr std::uint16_t byteTestAddress = 0x0064;
        static constexpr std::uint16_t hardwareConfigAddress = 0x0074;
        static constexpr std::uint32_t byteTestValue = 0x87654321;
        static constexpr std::uint32_t readyMask = 1U << 27;
        static constexpr std::uint16_t chipId = 0x9253;

        struct Identity
        {
            std::uint32_t byte_test{}, hardware_config{}, id_revision{};
            constexpr bool operator==(const Identity&) const = default;
        };

        Lan9253(ISpi& spi, IDigitalOutput& chip_select) noexcept;
        [[nodiscard]] hal::util::Result<std::uint32_t> readSystemRegister(
          std::uint16_t address, std::chrono::milliseconds timeout = std::chrono::milliseconds{ 100 });
        [[nodiscard]] hal::util::Result<> writeSystemRegister(
          std::uint16_t address, std::uint32_t value,
          std::chrono::milliseconds timeout = std::chrono::milliseconds{ 100 });
        // Indirect accesses use aligned sizes 1, 2 or 4, within 0x0000..0x2fff.
        // Process RAM (0x1000..0x2fff) is also accessible through this interface
        // (DS00003421B 11.13.1); FIFO transfers are a future optimization.
        // Before READY, only use these for the EEPROM-emulation bootstrap
        // described by DS00003421B section 13.4, after BYTE_TEST succeeds.
        [[nodiscard]] hal::util::Result<std::uint32_t> readEscRegister(
          std::uint16_t address, std::uint8_t size,
          std::chrono::milliseconds timeout = std::chrono::milliseconds{ 100 });
        [[nodiscard]] hal::util::Result<> writeEscRegister(
          std::uint16_t address, std::uint8_t size, std::uint32_t value,
          std::chrono::milliseconds timeout = std::chrono::milliseconds{ 100 });
        // Serve only startup configuration requests (words 0 and 4). This is
        // volatile configuration, NOT EEPROM programming or a complete SII
        // emulator. Rejects non-emulation mode, writes and other addresses.
        // Requires PDI 0x80 and a valid CRC over the first 14 bytes.
        [[nodiscard]] hal::util::Result<Identity> initializeEmulatedBoot(
          std::span<const std::uint8_t, 16> configuration,
          std::chrono::milliseconds timeout = std::chrono::milliseconds{ 1000 });
        // Single attempt: BYTE_TEST, then READY, then ID. Never reads ID while
        // not ready. The caller may retry within its own startup deadline.
        [[nodiscard]] hal::util::Result<Identity> identify(
          std::chrono::milliseconds timeout = std::chrono::milliseconds{ 100 });

      private:
        [[nodiscard]] hal::util::Result<> waitCsr(std::chrono::steady_clock::time_point deadline);
        ISpi& m_spi;
        IDigitalOutput& m_chipSelect;
    };
}
