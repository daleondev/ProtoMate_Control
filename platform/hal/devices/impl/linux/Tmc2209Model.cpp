#include "Tmc2209Model.hpp"
#include "hal/devices/impl/Tmc2209.hpp"
#include "hal/devices/util/Tmc2209Protocol.hpp"

namespace hal::device
{
    using namespace util::tmc2209;
    Tmc2209Model::Tmc2209Model()
    {
        reset(0);
        reset(1);
    }
    void Tmc2209Model::reset(unsigned address)
    {
        for (auto& reg : m_registers.at(address)) {
            reg = 0;
        }
        m_registers[address][0x00] = GCONF_RESET;
        m_registers[address][0x01] = 1;
        m_registers[address][IOIN] = IOIN_RESET | (address << 2U);
        m_registers[address][CHOPCONF] = CHOPCONF_RESET;
        m_registers[address][DRV_STATUS] = DRV_STATUS_RESET;
        m_registers[address][PWMCONF] = PWMCONF_DEFAULT;
    }
    void Tmc2209Model::setRegister(unsigned address, unsigned reg, std::uint32_t value)
    {
        m_registers.at(address).at(reg) = value;
    }
    std::uint32_t Tmc2209Model::getRegister(unsigned address, unsigned reg) const
    {
        return m_registers.at(address).at(reg);
    }
    void Tmc2209Model::setConnected(unsigned address, bool connected) { m_connected.at(address) = connected; }
    hal::util::Result<> Tmc2209Model::exchange(std::span<const std::uint8_t> request,
                                               std::span<std::uint8_t> reply,
                                               std::chrono::milliseconds /*timeout*/)
    {
        auto error = [](std::errc e) { return std::unexpected(std::make_error_code(e)); };
        if ((request.size() != READ_REQUEST_BYTES && request.size() != FRAME_BYTES) || request[0] != SYNC ||
            device::Tmc2209::crc(request.first(request.size() - 1)) != request.back()) {
            return error(std::errc::protocol_error);
        }
        const auto address = request[1];
        const auto reg = static_cast<std::uint8_t>(request[2] & REGISTER_MASK);
        if (address >= 2 || !m_connected[address]) {
            if (reply.empty() && request.size() == FRAME_BYTES) {
                return {};
            }
            return error(std::errc::timed_out);
        }
        if (request[2] & WRITE_FLAG) {
            if (request.size() != FRAME_BYTES || !reply.empty()) {
                return error(std::errc::protocol_error);
            }
            auto value = read_word(request.subspan<DATA_OFFSET, REGISTER_BYTES>());
            if (reg == 1) {
                m_registers[address][reg].fetch_and(~value);
            }
            else {
                m_registers[address][reg] = value;
            }
            m_registers[address][2] = (m_registers[address][2] + 1U) & MASTER_ADDRESS;
            return {};
        }
        if (request.size() != READ_REQUEST_BYTES || reply.size() != FRAME_BYTES) {
            return error(std::errc::protocol_error);
        }
        auto value = m_registers[address][reg].load();
        reply[0] = SYNC;
        reply[1] = MASTER_ADDRESS;
        reply[2] = reg;
        write_word(reply.subspan<DATA_OFFSET, REGISTER_BYTES>(), value);
        reply.back() = device::Tmc2209::crc(reply.first(reply.size() - 1U));
        return {};
    }
}
