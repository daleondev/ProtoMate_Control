#include "St1633i.hpp"
#include <thread>
#include <chrono>
#include <utility>
namespace hal::device {
    St1633i::St1633i(std::shared_ptr<II2c> bus, std::uint8_t address,
        std::shared_ptr<IDigitalOutput> reset, std::shared_ptr<IDigitalInput> interrupt)
      : m_bus(std::move(bus)), m_address(address),
        m_reset(std::move(reset)), m_interrupt(std::move(interrupt)) {}
    auto St1633i::initialize() -> util::Result<>
    {
        m_reset->write(gpio::Level::Low);
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        m_reset->write(gpio::Level::High);
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        auto result = m_bus->probe(m_address, std::chrono::milliseconds{20});
        if (result) { m_initialized = true; }
        return result;
    }
    auto St1633i::read() -> util::Result<TouchReport>
    {
        if (!m_initialized) { return std::unexpected(std::make_error_code(std::errc::not_connected)); }
        std::array<std::uint8_t, 2> identity{};
        auto result = m_bus->readRegister(m_address, 0, identity, std::chrono::milliseconds{20});
        if (!result) { return std::unexpected(result.error()); }
        if ((identity[1] & 0xFu) != 0 && (identity[1] & 0xFu) != 4) {
            return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
        }
        std::array<std::uint8_t, 20> bytes{};
        result = m_bus->readRegister(m_address, 0x12, bytes, std::chrono::milliseconds{20});
        if (!result) { return std::unexpected(result.error()); }
        TouchReport report{.firmware = identity[0], .status = identity[1]};
        for (unsigned i = 0; i < report.points.size(); ++i) {
            const auto offset = i * 4;
            report.points[i] = {.active = (bytes[offset] & 0x80u) != 0,
                .x = static_cast<std::uint16_t>(((bytes[offset] & 0x70u) << 4) | bytes[offset+1]),
                .y = static_cast<std::uint16_t>(((bytes[offset] & 7u) << 8) | bytes[offset+2])};
        }
        return report;
    }
}
