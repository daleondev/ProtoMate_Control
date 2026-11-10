#include "St1633i.hpp"
#include <chrono>
#include <thread>
#include <utility>
namespace hal::device
{
    namespace
    {
        constexpr auto RESET_HOLD{ std::chrono::milliseconds{ 2 } };
        constexpr auto STARTUP_DELAY{ std::chrono::milliseconds{ 100 } };
        constexpr auto PROBE_TIMEOUT{ std::chrono::milliseconds{ 2 } };
        constexpr auto TRANSFER_TIMEOUT{ std::chrono::milliseconds{ 5 } };
        constexpr unsigned STATUS_MASK{ 0xF }, NORMAL_STATUS{ 4 };
        constexpr unsigned POINT_REGISTER{ 0x12 }, POINT_BYTES{ 4 };
        constexpr unsigned ACTIVE_MASK{ 0x80 }, X_HIGH_MASK{ 0x70 }, Y_HIGH_MASK{ 7 };
        constexpr unsigned X_HIGH_SHIFT{ 4 }, Y_HIGH_SHIFT{ 8 };
    }
    St1633i::~St1633i()
    {
        if (m_interrupt) {
            m_interrupt->clearEdgeCallback();
        }
    }
    void St1633i::setCallback(Callback callback, void* context) noexcept
    {
        if (!m_interrupt) {
            return;
        }
        m_interrupt->clearEdgeCallback();
        if (callback) {
            m_interrupt->setEdgeCallback([callback, context](gpio::Level) noexcept { callback(context); });
        }
    }
    St1633i::St1633i(std::shared_ptr<II2c> bus,
                     std::uint8_t address,
                     std::shared_ptr<IDigitalOutput> reset,
                     std::shared_ptr<IDigitalInput> interrupt)
      : m_bus{ std::move(bus) }
      , m_address{ address }
      , m_reset{ std::move(reset) }
      , m_interrupt{ std::move(interrupt) }
    {
    }
    auto St1633i::initialize() -> util::Result<>
    {
        m_initialized = false;
        m_reset->write(gpio::Level::Low);
        std::this_thread::sleep_for(RESET_HOLD);
        m_reset->write(gpio::Level::High);
        std::this_thread::sleep_for(STARTUP_DELAY);
        auto result{ m_bus->probe(m_address, PROBE_TIMEOUT) };
        if (result) {
            m_initialized = true;
        }
        return result;
    }
    auto St1633i::read() -> util::Result<TouchReport>
    {
        if (!m_initialized) {
            return std::unexpected(std::make_error_code(std::errc::not_connected));
        }
        std::array<std::uint8_t, 2> identity{};
        auto result{ m_bus->readRegister(m_address, 0, identity, TRANSFER_TIMEOUT) };
        if (!result) {
            return std::unexpected(result.error());
        }
        if ((identity[1] & STATUS_MASK) != 0 && (identity[1] & STATUS_MASK) != NORMAL_STATUS) {
            return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
        }
        std::array<std::uint8_t, TouchReport{}.points.size() * POINT_BYTES> bytes{};
        result = m_bus->readRegister(m_address, POINT_REGISTER, bytes, TRANSFER_TIMEOUT);
        if (!result) {
            return std::unexpected(result.error());
        }
        TouchReport report{ .firmware = identity[0], .status = identity[1] };
        for (unsigned i{ 0 }; i < report.points.size(); ++i) {
            const auto offset{ i * POINT_BYTES };
            report.points[i] = { .active = (bytes[offset] & ACTIVE_MASK) != 0,
                                 .x = static_cast<std::uint16_t>(
                                   ((bytes[offset] & X_HIGH_MASK) << X_HIGH_SHIFT) | bytes[offset + 1]),
                                 .y = static_cast<std::uint16_t>(
                                   ((bytes[offset] & Y_HIGH_MASK) << Y_HIGH_SHIFT) | bytes[offset + 2]) };
        }
        return report;
    }
}
