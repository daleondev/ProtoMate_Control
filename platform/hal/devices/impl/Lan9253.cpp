#include "Lan9253.hpp"
#include <array>
#include <limits>
#include <thread>

namespace
{
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    constexpr std::uint16_t CSR_DATA = 0x300, CSR_COMMAND = 0x304;
    constexpr std::uint32_t CSR_BUSY = 1U << 31, CSR_READ = 1U << 30;

    constexpr unsigned BYTE_BITS{ 8 };
    constexpr unsigned DWORD_BYTES{ sizeof(std::uint32_t) };
    constexpr unsigned WORD_BYTES{ sizeof(std::uint16_t) };
    constexpr unsigned HEADER_BYTES{ 3 };
    constexpr unsigned SIZE_SHIFT{ 16 };
    constexpr unsigned CHIP_ID_SHIFT{ 16 };
    constexpr std::uint16_t SYSTEM_REGISTER_FIRST{ 0x40 };
    constexpr std::uint16_t SYSTEM_REGISTER_LAST{ 0x3fc };
    constexpr std::uint16_t ESC_REGISTER_LIMIT{ 0x3000 };
    constexpr std::uint8_t SPI_READ{ 0x03 };
    constexpr std::uint8_t SPI_WRITE{ 0x02 };
    constexpr std::uint8_t PREFETCH_END{ 0xff };
    constexpr std::uint8_t CRC_INITIAL{ 0xff };
    constexpr std::uint8_t CRC_HIGH_BIT{ 0x80 };
    constexpr std::uint8_t CRC_POLYNOMIAL{ 0x07 };
    constexpr std::uint8_t PDI_SPI{ 0x80 };
    constexpr unsigned CONFIGURATION_CRC_INDEX{ 14 };
    constexpr std::uint16_t EEPROM_STATUS{ 0x502 };
    constexpr std::uint16_t EEPROM_ADDRESS{ 0x504 };
    constexpr std::uint16_t EEPROM_DATA{ 0x508 };
    constexpr std::uint32_t EEPROM_EMULATED{ 0x20 };
    constexpr std::uint32_t EEPROM_BUSY{ 0x8000 };
    constexpr std::uint32_t EEPROM_COMMAND_MASK{ 0x0700 };
    constexpr std::uint32_t EEPROM_READ{ 0x0100 };
    constexpr std::uint32_t EEPROM_RELOAD{ 0x0400 };
    constexpr unsigned EEPROM_REQUEST_BYTES{ 8 };

    std::uint32_t read_dword(std::span<const std::uint8_t> bytes)
    {
        std::uint32_t value{};
        for (unsigned index{}; index < DWORD_BYTES; ++index) {
            value |= std::uint32_t{ bytes[index] } << (index * BYTE_BITS);
        }
        return value;
    }

    bool valid_timeout(std::chrono::milliseconds timeout)
    {
        return timeout > 0ms && timeout <= hal::device::Lan9253::BOOT_TIMEOUT;
    }

    std::chrono::milliseconds remaining(Clock::time_point deadline)
    {
        return std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
    }

    bool valid_csr(std::uint16_t address, std::uint8_t size)
    {
        return (size == 1 || size == 2 || size == 4) && address < ESC_REGISTER_LIMIT && address % size == 0;
    }

    struct Select
    {
        Select(const Select&) = delete;
        auto operator=(const Select&) -> Select& = delete;
        Select(Select&&) = delete;
        auto operator=(Select&&) -> Select& = delete;

        hal::IDigitalOutput& pin; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members) -- Required,
                                  // non-owning dependency; its owner outlives this object.
        explicit Select(hal::IDigitalOutput& p)
          : pin{ p }
        {
            pin.write(hal::gpio::Level::Low);
        }
        ~Select() { pin.write(hal::gpio::Level::High); }
    };

    std::uint8_t configuration_crc(std::span<const std::uint8_t> bytes)
    {
        std::uint8_t crc = CRC_INITIAL;
        for (const auto byte : bytes) {
            crc ^= byte;
            for (unsigned bit = 0; bit < BYTE_BITS; ++bit)
                crc = static_cast<std::uint8_t>((crc << 1) ^ ((crc & CRC_HIGH_BIT) ? CRC_POLYNOMIAL : 0));
        }
        return crc;
    }
}

namespace hal::device
{
    Lan9253::Lan9253(ISpi& spi, IDigitalOutput& chip_select) noexcept
      : m_spi{ spi }
      , m_chipSelect{ chip_select }
    {
        m_chipSelect.write(gpio::Level::High);
    }

    hal::util::Result<std::uint32_t> Lan9253::readSystemRegister(std::uint16_t address,
                                                                 std::chrono::milliseconds timeout)
    {
        if ((address & 3U) != 0 || address < SYSTEM_REGISTER_FIRST || address > SYSTEM_REGISTER_LAST ||
            !valid_timeout(timeout))
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));

        // DS00003421B section 9.2.5.1: address MSB first, data LSB first;
        // zero MOSI for every data byte except the last (FF terminates prefetch).
        const std::array<std::uint8_t, HEADER_BYTES + DWORD_BYTES> tx{ SPI_READ,
                                                                       static_cast<std::uint8_t>(address >>
                                                                                                 BYTE_BITS),
                                                                       static_cast<std::uint8_t>(address),
                                                                       0x00,
                                                                       0x00,
                                                                       0x00,
                                                                       PREFETCH_END };
        std::array<std::uint8_t, HEADER_BYTES + DWORD_BYTES> rx{};
        Select selected{ m_chipSelect };
        if (auto result = m_spi.exchange(tx, rx, timeout); !result)
            return std::unexpected(result.error());
        return read_dword(std::span{ rx }.subspan(HEADER_BYTES));
    }

    // Register API keeps the conventional address, optional width, value order.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    hal::util::Result<> Lan9253::writeSystemRegister(std::uint16_t address,
                                                     std::uint32_t value,
                                                     std::chrono::milliseconds timeout)
    {
        if ((address & 3U) != 0 || address < SYSTEM_REGISTER_FIRST || address > SYSTEM_REGISTER_LAST ||
            !valid_timeout(timeout))
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        std::array<std::uint8_t, HEADER_BYTES + DWORD_BYTES> tx{
            SPI_WRITE, static_cast<std::uint8_t>(address >> BYTE_BITS), static_cast<std::uint8_t>(address)
        };
        for (unsigned index{}; index < DWORD_BYTES; ++index) {
            tx[HEADER_BYTES + index] = static_cast<std::uint8_t>(value >> (index * BYTE_BITS));
        }
        std::array<std::uint8_t, HEADER_BYTES + DWORD_BYTES> rx{};
        Select selected{ m_chipSelect };
        return m_spi.exchange(tx, rx, timeout);
    }
    // NOLINTEND(bugprone-easily-swappable-parameters)

    hal::util::Result<> Lan9253::waitCsr(Clock::time_point deadline)
    {
        while (Clock::now() < deadline) {
            const auto budget = remaining(deadline);
            if (budget <= 0ms)
                break;
            const auto command = readSystemRegister(CSR_COMMAND, budget);
            if (!command)
                return std::unexpected(command.error());
            if ((*command & CSR_BUSY) == 0)
                return {};
            std::this_thread::yield();
        }
        return std::unexpected(std::make_error_code(std::errc::timed_out));
    }

    hal::util::Result<std::uint32_t> Lan9253::readEscRegister(std::uint16_t address,
                                                              std::uint8_t size,
                                                              std::chrono::milliseconds timeout)
    {
        if (!valid_csr(address, size) || !valid_timeout(timeout))
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        const auto deadline = Clock::now() + timeout;
        if (auto idle = waitCsr(deadline); !idle)
            return std::unexpected(idle.error());
        timeout = remaining(deadline);
        if (timeout <= 0ms)
            return std::unexpected(std::make_error_code(std::errc::timed_out));
        if (auto sent = writeSystemRegister(
              CSR_COMMAND, CSR_BUSY | CSR_READ | (std::uint32_t{ size } << SIZE_SHIFT) | address, timeout);
            !sent)
            return std::unexpected(sent.error());
        if (auto idle = waitCsr(deadline); !idle)
            return std::unexpected(idle.error());
        timeout = remaining(deadline);
        if (timeout <= 0ms)
            return std::unexpected(std::make_error_code(std::errc::timed_out));
        const auto value = readSystemRegister(CSR_DATA, timeout);
        if (!value)
            return std::unexpected(value.error());
        // The other bytes in CSR_DATA also update; only requested bytes count.
        return *value & (std::numeric_limits<std::uint32_t>::max() >> ((DWORD_BYTES - size) * BYTE_BITS));
    }

    // Register API keeps the conventional address, optional width, value order.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    hal::util::Result<> Lan9253::writeEscRegister(std::uint16_t address,
                                                  std::uint8_t size,
                                                  std::uint32_t value,
                                                  std::chrono::milliseconds timeout)
    {
        if (!valid_csr(address, size) || !valid_timeout(timeout))
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        const auto deadline = Clock::now() + timeout;
        if (auto idle = waitCsr(deadline); !idle)
            return idle;
        timeout = remaining(deadline);
        if (timeout <= 0ms)
            return std::unexpected(std::make_error_code(std::errc::timed_out));
        if (auto sent = writeSystemRegister(CSR_DATA, value, timeout); !sent)
            return sent;
        timeout = remaining(deadline);
        if (timeout <= 0ms)
            return std::unexpected(std::make_error_code(std::errc::timed_out));
        if (auto sent = writeSystemRegister(
              CSR_COMMAND, CSR_BUSY | (std::uint32_t{ size } << SIZE_SHIFT) | address, timeout);
            !sent)
            return sent;
        return waitCsr(deadline);
    }
    // NOLINTEND(bugprone-easily-swappable-parameters)

    hal::util::Result<Lan9253::Identity> Lan9253::initializeEmulatedBoot(
      std::span<const std::uint8_t, CONFIGURATION_BYTES> configuration,
      std::chrono::milliseconds timeout)
    {
        if (!valid_timeout(timeout) || configuration[0] != PDI_SPI || configuration.back() != 0 ||
            configuration_crc(configuration.first<CONFIGURATION_CRC_INDEX>()) !=
              configuration[CONFIGURATION_CRC_INDEX])
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            const auto budget = remaining(deadline);
            if (budget <= 0ms)
                break;
            const auto identity = identify(budget);
            if (identity)
                return identity;
            if (identity.error() != std::errc::resource_unavailable_try_again)
                return std::unexpected(identity.error());

            if (auto serviced = serviceEmulatedRequest(configuration, deadline); !serviced)
                return std::unexpected(serviced.error());
        }
        return std::unexpected(std::make_error_code(std::errc::timed_out));
    }

    hal::util::Result<> Lan9253::serviceEmulatedRequest(
      std::span<const std::uint8_t, CONFIGURATION_BYTES> configuration,
      Clock::time_point deadline)
    {
        auto read = [&](std::uint16_t address, std::uint8_t size) -> hal::util::Result<std::uint32_t> {
            const auto budget = remaining(deadline);
            if (budget <= 0ms)
                return std::unexpected(std::make_error_code(std::errc::timed_out));
            return readEscRegister(address, size, budget);
        };
        auto write = [&](std::uint16_t address, std::uint8_t size, std::uint32_t value)
          -> hal::util::Result<> { // NOLINT(bugprone-easily-swappable-parameters) -- Register API order is
                                   // address, width (if present), then value.
            const auto budget = remaining(deadline);
            if (budget <= 0ms)
                return std::unexpected(std::make_error_code(std::errc::timed_out));
            return writeEscRegister(address, size, value, budget);
        };
        // EEPROM emulation specifically enables the PDI before configuration
        // is loaded (DS00003421B 12.2/13.4). Without servicing these requests,
        // waiting for READY deadlocks. Normal, non-emulated startup still
        // follows the strict READY ordering in identify().
        const auto status = read(EEPROM_STATUS, WORD_BYTES);
        if (!status)
            return std::unexpected(status.error());
        if ((*status & EEPROM_EMULATED) == 0)
            return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
        if ((*status & EEPROM_BUSY) == 0) {
            std::this_thread::sleep_for(1ms);
            return {};
        }
        const auto command = *status & EEPROM_COMMAND_MASK;
        if (command != EEPROM_READ && command != EEPROM_RELOAD)
            return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
        const auto address = read(EEPROM_ADDRESS, DWORD_BYTES);
        if (!address)
            return std::unexpected(address.error());
        if (*address != 0 && *address != EEPROM_REQUEST_BYTES / WORD_BYTES)
            return std::unexpected(std::make_error_code(std::errc::result_out_of_range));
        // The address counts WORDs. Each request consumes 8 bytes.
        for (unsigned offset = 0; offset < EEPROM_REQUEST_BYTES; offset += DWORD_BYTES) {
            const auto data = configuration.subspan((*address * WORD_BYTES) + offset, DWORD_BYTES);
            const auto value = read_dword(data);
            if (auto sent = write(EEPROM_DATA + offset, DWORD_BYTES, value); !sent)
                return std::unexpected(sent.error());
        }
        // Acknowledge just the executed command; never copy status/error
        // bits back, and never issue an EEPROM WRITE or trigger a reload.
        if (auto sent = write(EEPROM_STATUS, WORD_BYTES, command); !sent)
            return std::unexpected(sent.error());
        return {};
    }

    hal::util::Result<Lan9253::Identity> Lan9253::identify(std::chrono::milliseconds timeout)
    {
        if (!valid_timeout(timeout))
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        auto read = [&](std::uint16_t address) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return hal::util::Result<std::uint32_t>{ std::unexpected(
                  std::make_error_code(std::errc::timed_out)) };
            return readSystemRegister(address, std::chrono::ceil<std::chrono::milliseconds>(deadline - now));
        };
        const auto byte_test = read(BYTE_TEST_ADDRESS);
        if (!byte_test)
            return std::unexpected(byte_test.error());
        if (*byte_test != BYTE_TEST_VALUE)
            return std::unexpected(std::make_error_code(std::errc::protocol_error));
        const auto config = read(HARDWARE_CONFIG_ADDRESS);
        if (!config)
            return std::unexpected(config.error());
        if ((*config & READY_MASK) == 0)
            return std::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
        const auto id = read(ID_REVISION_ADDRESS);
        if (!id)
            return std::unexpected(id.error());
        if ((*id >> CHIP_ID_SHIFT) != CHIP_ID)
            return std::unexpected(std::make_error_code(std::errc::no_such_device));
        return Identity{ .byte_test = *byte_test, .hardware_config = *config, .id_revision = *id };
    }
}
