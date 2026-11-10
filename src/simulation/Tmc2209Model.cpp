#include "Tmc2209Model.hpp"

#include "hal/devices/util/Tmc2209Protocol.hpp"
#include <algorithm>

namespace hal::device
{
    using namespace util::tmc2209;
    namespace
    {
        std::uint8_t crc(std::span<const std::uint8_t> bytes)
        {
            std::uint8_t result{};
            for (auto byte : bytes) {
                for (unsigned i{}; i < BYTE_BITS; ++i) {
                    const bool bit{ bool((result >> (BYTE_BITS - 1)) ^ (byte & 1U)) };
                    result = static_cast<std::uint8_t>(result << 1U);
                    if (bit) {
                        result ^= CRC_POLYNOMIAL;
                    }
                    byte >>= 1U;
                }
            }
            return result;
        }
    }
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
        m_registers[address][GCONF] = GCONF_RESET;
        m_registers[address][GSTAT] = 1;
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
    unsigned Tmc2209Model::microsteps(unsigned address) const
    {
        if (!(getRegister(address, GCONF) & GCONF_MSTEP_REG_SELECT)) {
            // MS2/MS1 also select the UART address. They remain authoritative
            // after reset until firmware explicitly selects the MRES register.
            constexpr std::array<unsigned, 4> pin_resolution{ 8, 32, 64, 16 };
            return pin_resolution[(getRegister(address, IOIN) >> 2U) & 3U];
        }
        const auto mres{ (getRegister(address, CHOPCONF) >> MRES_SHIFT) & MRES_MASK };
        return MAX_MICROSTEPS >> std::min(MRES_EXPONENT, mres);
    }
    void Tmc2209Model::setConnected(unsigned address, bool connected) { m_connected.at(address) = connected; }
    hal::util::Result<> Tmc2209Model::exchange(std::span<const std::uint8_t> request,
                                               std::span<std::uint8_t> reply,
                                               std::chrono::milliseconds /*timeout*/)
    {
        auto error{ [](std::errc e) { return std::unexpected(std::make_error_code(e)); } };
        if ((request.size() != READ_REQUEST_BYTES && request.size() != FRAME_BYTES) || request[0] != SYNC ||
            crc(request.first(request.size() - 1)) != request.back()) {
            return error(std::errc::protocol_error);
        }
        const auto address{ request[1] };
        const auto reg{ static_cast<std::uint8_t>(request[2] & REGISTER_MASK) };
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
            auto value{ read_word(request.subspan<DATA_OFFSET, REGISTER_BYTES>()) };
            if (reg == GSTAT) {
                m_registers[address][reg].fetch_and(~value);
            }
            else {
                switch (reg) {
                    case GCONF:
                    case NODECONF:
                    case FACTORY_CONF:
                    case IHOLD_IRUN:
                    case TPOWERDOWN:
                    case TPWMTHRS:
                    case TCOOLTHRS:
                    case VACTUAL:
                    case SGTHRS:
                    case COOLCONF:
                    case CHOPCONF:
                    case PWMCONF:
                        m_registers[address][reg] = value;
                        break;
                    default:
                        return {}; // Read-only/reserved writes do not acknowledge through IFCNT.
                }
            }
            m_registers[address][IFCNT] = (m_registers[address][IFCNT] + 1U) & MASTER_ADDRESS;
            return {};
        }
        if (request.size() != READ_REQUEST_BYTES || reply.size() != FRAME_BYTES) {
            return error(std::errc::protocol_error);
        }
        auto value{ m_registers[address][reg].load() };
        reply[0] = SYNC;
        reply[1] = MASTER_ADDRESS;
        reply[2] = reg;
        write_word(reply.subspan<DATA_OFFSET, REGISTER_BYTES>(), value);
        reply.back() = crc(reply.first(reply.size() - 1U));
        return {};
    }
}
