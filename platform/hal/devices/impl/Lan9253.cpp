#include "Lan9253.hpp"
#include <array>
#include <thread>

namespace
{
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    constexpr std::uint16_t csrData = 0x300, csrCommand = 0x304;
    constexpr std::uint32_t csrBusy = 1U << 31, csrRead = 1U << 30;

    bool validTimeout(std::chrono::milliseconds timeout)
    {
        return timeout > 0ms && timeout <= 1000ms;
    }

    std::chrono::milliseconds remaining(Clock::time_point deadline)
    {
        return std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
    }

    bool validCsr(std::uint16_t address, std::uint8_t size)
    {
        return (size == 1 || size == 2 || size == 4) && address < 0x3000 && address % size == 0;
    }

    struct Select
    {
        hal::IDigitalOutput& pin;
        explicit Select(hal::IDigitalOutput& p) : pin{ p } { pin.write(hal::gpio::Level::Low); }
        ~Select() { pin.write(hal::gpio::Level::High); }
    };

    std::uint8_t configurationCrc(std::span<const std::uint8_t> bytes)
    {
        std::uint8_t crc = 0xff;
        for (const auto byte : bytes) {
            crc ^= byte;
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = static_cast<std::uint8_t>((crc << 1) ^ ((crc & 0x80) ? 0x07 : 0));
        }
        return crc;
    }
}

namespace hal::device
{
    Lan9253::Lan9253(ISpi& spi, IDigitalOutput& chip_select) noexcept
      : m_spi{ spi }, m_chipSelect{ chip_select }
    {
        m_chipSelect.write(gpio::Level::High);
    }

    hal::util::Result<std::uint32_t> Lan9253::readSystemRegister(
      std::uint16_t address, std::chrono::milliseconds timeout)
    {
        if ((address & 3U) != 0 || address < 0x40U || address > 0x3fcU ||
            timeout.count() <= 0 || timeout.count() > 1000)
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));

        // DS00003421B section 9.2.5.1: address MSB first, data LSB first;
        // zero MOSI for every data byte except the last (FF terminates prefetch).
        const std::array<std::uint8_t, 7> tx{
            0x03, static_cast<std::uint8_t>(address >> 8), static_cast<std::uint8_t>(address),
            0x00, 0x00, 0x00, 0xff };
        std::array<std::uint8_t, 7> rx{};
        Select selected{ m_chipSelect };
        if (auto result = m_spi.exchange(tx, rx, timeout); !result)
            return std::unexpected(result.error());
        return std::uint32_t{ rx[3] } | (std::uint32_t{ rx[4] } << 8) |
               (std::uint32_t{ rx[5] } << 16) | (std::uint32_t{ rx[6] } << 24);
    }

    hal::util::Result<> Lan9253::writeSystemRegister(
      std::uint16_t address, std::uint32_t value, std::chrono::milliseconds timeout)
    {
        if ((address & 3U) != 0 || address < 0x40U || address > 0x3fcU || !validTimeout(timeout))
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        const std::array<std::uint8_t, 7> tx{
            0x02, static_cast<std::uint8_t>(address >> 8), static_cast<std::uint8_t>(address),
            static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
            static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24) };
        std::array<std::uint8_t, 7> rx{};
        Select selected{ m_chipSelect };
        return m_spi.exchange(tx, rx, timeout);
    }

    hal::util::Result<> Lan9253::waitCsr(Clock::time_point deadline)
    {
        while (Clock::now() < deadline) {
            const auto budget = remaining(deadline);
            if (budget <= 0ms) break;
            const auto command = readSystemRegister(csrCommand, budget);
            if (!command) return std::unexpected(command.error());
            if ((*command & csrBusy) == 0) return {};
            std::this_thread::yield();
        }
        return std::unexpected(std::make_error_code(std::errc::timed_out));
    }

    hal::util::Result<std::uint32_t> Lan9253::readEscRegister(
      std::uint16_t address, std::uint8_t size, std::chrono::milliseconds timeout)
    {
        if (!validCsr(address, size) || !validTimeout(timeout))
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        const auto deadline = Clock::now() + timeout;
        if (auto idle = waitCsr(deadline); !idle) return std::unexpected(idle.error());
        timeout = remaining(deadline);
        if (timeout <= 0ms) return std::unexpected(std::make_error_code(std::errc::timed_out));
        if (auto sent = writeSystemRegister(csrCommand, csrBusy | csrRead | (std::uint32_t{ size } << 16) | address,
                                            timeout); !sent)
            return std::unexpected(sent.error());
        if (auto idle = waitCsr(deadline); !idle) return std::unexpected(idle.error());
        timeout = remaining(deadline);
        if (timeout <= 0ms) return std::unexpected(std::make_error_code(std::errc::timed_out));
        const auto value = readSystemRegister(csrData, timeout);
        if (!value) return std::unexpected(value.error());
        // The other bytes in CSR_DATA also update; only requested bytes count.
        return *value & (0xffffffffU >> ((4U - size) * 8U));
    }

    hal::util::Result<> Lan9253::writeEscRegister(
      std::uint16_t address, std::uint8_t size, std::uint32_t value, std::chrono::milliseconds timeout)
    {
        if (!validCsr(address, size) || !validTimeout(timeout))
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        const auto deadline = Clock::now() + timeout;
        if (auto idle = waitCsr(deadline); !idle) return idle;
        timeout = remaining(deadline);
        if (timeout <= 0ms) return std::unexpected(std::make_error_code(std::errc::timed_out));
        if (auto sent = writeSystemRegister(csrData, value, timeout); !sent) return sent;
        timeout = remaining(deadline);
        if (timeout <= 0ms) return std::unexpected(std::make_error_code(std::errc::timed_out));
        if (auto sent = writeSystemRegister(csrCommand, csrBusy | (std::uint32_t{ size } << 16) | address,
                                            timeout); !sent) return sent;
        return waitCsr(deadline);
    }

    hal::util::Result<Lan9253::Identity> Lan9253::initializeEmulatedBoot(
      std::span<const std::uint8_t, 16> configuration, std::chrono::milliseconds timeout)
    {
        if (!validTimeout(timeout) || configuration[0] != 0x80 || configuration[15] != 0 ||
            configurationCrc(configuration.first<14>()) != configuration[14])
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        const auto deadline = Clock::now() + timeout;
        auto read = [&](std::uint16_t address, std::uint8_t size) -> hal::util::Result<std::uint32_t> {
            const auto budget = remaining(deadline);
            if (budget <= 0ms) return std::unexpected(std::make_error_code(std::errc::timed_out));
            return readEscRegister(address, size, budget);
        };
        auto write = [&](std::uint16_t address, std::uint8_t size, std::uint32_t value) -> hal::util::Result<> {
            const auto budget = remaining(deadline);
            if (budget <= 0ms) return std::unexpected(std::make_error_code(std::errc::timed_out));
            return writeEscRegister(address, size, value, budget);
        };
        while (Clock::now() < deadline) {
            const auto budget = remaining(deadline);
            if (budget <= 0ms) break;
            const auto identity = identify(budget);
            if (identity) return identity;
            if (identity.error() != std::errc::resource_unavailable_try_again)
                return std::unexpected(identity.error());

            // EEPROM emulation specifically enables the PDI before configuration
            // is loaded (DS00003421B 12.2/13.4). Without servicing these requests,
            // waiting for READY deadlocks. Normal, non-emulated startup still
            // follows the strict READY ordering in identify().
            const auto status = read(0x502, 2);
            if (!status) return std::unexpected(status.error());
            if ((*status & 0x20U) == 0)
                return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
            if ((*status & 0x8000U) == 0) {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            const auto command = *status & 0x0700U;
            if (command != 0x0100U && command != 0x0400U)
                return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
            const auto address = read(0x504, 4);
            if (!address) return std::unexpected(address.error());
            if (*address != 0 && *address != 4)
                return std::unexpected(std::make_error_code(std::errc::result_out_of_range));
            // The address counts WORDs. Each request consumes 8 bytes.
            for (unsigned offset = 0; offset < 8; offset += 4) {
                const auto data = configuration.subspan(*address * 2U + offset, 4);
                const std::uint32_t value = std::uint32_t{ data[0] } | (std::uint32_t{ data[1] } << 8) |
                                            (std::uint32_t{ data[2] } << 16) | (std::uint32_t{ data[3] } << 24);
                if (auto sent = write(0x508 + offset, 4, value); !sent) return std::unexpected(sent.error());
            }
            // Acknowledge just the executed command; never copy status/error
            // bits back, and never issue an EEPROM WRITE or trigger a reload.
            if (auto sent = write(0x502, 2, command); !sent) return std::unexpected(sent.error());
        }
        return std::unexpected(std::make_error_code(std::errc::timed_out));
    }

    hal::util::Result<Lan9253::Identity> Lan9253::identify(std::chrono::milliseconds timeout)
    {
        if (timeout.count() <= 0 || timeout.count() > 1000)
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        auto read = [&](std::uint16_t address) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return hal::util::Result<std::uint32_t>{ std::unexpected(std::make_error_code(std::errc::timed_out)) };
            return readSystemRegister(address, std::chrono::ceil<std::chrono::milliseconds>(deadline - now));
        };
        const auto byte_test = read(byteTestAddress);
        if (!byte_test) return std::unexpected(byte_test.error());
        if (*byte_test != byteTestValue)
            return std::unexpected(std::make_error_code(std::errc::protocol_error));
        const auto config = read(hardwareConfigAddress);
        if (!config) return std::unexpected(config.error());
        if ((*config & readyMask) == 0)
            return std::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
        const auto id = read(idRevisionAddress);
        if (!id) return std::unexpected(id.error());
        if ((*id >> 16) != chipId)
            return std::unexpected(std::make_error_code(std::errc::no_such_device));
        return Identity{ *byte_test, *config, *id };
    }
}
