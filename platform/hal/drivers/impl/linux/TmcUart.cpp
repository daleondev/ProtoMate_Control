#include "TmcUart.hpp"
#include "hal/devices/impl/Tmc2209.hpp"
#include "hal/drivers/factory/uart.hpp"

namespace hal::uart
{
    namespace
    {
        std::weak_ptr<TmcUart> owner;
    }
    TmcUart::TmcUart()
    {
        reset(0);
        reset(1);
    }
    void TmcUart::reset(unsigned address)
    {
        for (auto& reg : m_registers.at(address))
            reg = 0;
        m_registers[address][0x00] = 0x101;
        m_registers[address][0x01] = 1;
        m_registers[address][0x06] = 0x21000001 | (address << 2U);
        m_registers[address][0x6C] = 0x10000053;
        m_registers[address][0x6F] = 0x80000000;
        m_registers[address][0x70] = 0xC10D0024;
    }
    void TmcUart::setRegister(unsigned address, unsigned reg, std::uint32_t value)
    {
        m_registers.at(address).at(reg) = value;
    }
    std::uint32_t TmcUart::getRegister(unsigned address, unsigned reg) const
    {
        return m_registers.at(address).at(reg);
    }
    void TmcUart::setConnected(unsigned address, bool connected) { m_connected.at(address) = connected; }
    util::Result<> TmcUart::exchange(std::span<const std::uint8_t> request,
                                     std::span<std::uint8_t> reply,
                                     std::chrono::milliseconds)
    {
        auto error = [](std::errc e) { return std::unexpected(std::make_error_code(e)); };
        if ((request.size() != 4 && request.size() != 8) || request[0] != 5 ||
            device::Tmc2209::crc(request.first(request.size() - 1)) != request.back())
            return error(std::errc::protocol_error);
        const auto address = request[1], reg = static_cast<std::uint8_t>(request[2] & 0x7F);
        if (address >= 2 || !m_connected[address]) {
            if (reply.empty() && request.size() == 8)
                return {};
            return error(std::errc::timed_out);
        }
        if (request[2] & 0x80U) {
            if (request.size() != 8 || !reply.empty())
                return error(std::errc::protocol_error);
            auto value = (std::uint32_t{ request[3] } << 24U) | (std::uint32_t{ request[4] } << 16U) |
                         (std::uint32_t{ request[5] } << 8U) | request[6];
            if (reg == 1)
                m_registers[address][reg].fetch_and(~value);
            else
                m_registers[address][reg] = value;
            m_registers[address][2] = (m_registers[address][2] + 1U) & 0xFFU;
            return {};
        }
        if (request.size() != 4 || reply.size() != 8)
            return error(std::errc::protocol_error);
        auto value = m_registers[address][reg].load();
        reply[0] = 5;
        reply[1] = 0xFF;
        reply[2] = reg;
        for (unsigned i{}; i < 4; ++i)
            reply[3 + i] = value >> (24U - 8U * i);
        reply[7] = device::Tmc2209::crc(reply.first(7));
        return {};
    }
    std::shared_ptr<IUart> createStepperBus()
    {
        if (!owner.expired())
            return {};
        auto bus = std::make_shared<TmcUart>();
        owner = bus;
        return bus;
    }
    std::shared_ptr<TmcUart> simulatedStepperBus() { return owner.lock(); }
}
