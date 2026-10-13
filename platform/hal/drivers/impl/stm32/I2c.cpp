#include "I2c.hpp"
#include "hal/stm32/HalResult.hpp"
#include "hal/drivers/util/TransferValidation.hpp"
namespace hal {
    auto I2c::probe(std::uint8_t address, std::chrono::milliseconds timeout) -> util::Result<>
    {
        if (__get_IPSR() || address > 0x7F || !util::valid_transfer_timeout(timeout)) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        return make_result(HAL_I2C_IsDeviceReady(&m_handle, address << 1, 1, timeout.count()));
    }
    auto I2c::readRegister(std::uint8_t address, std::uint8_t reg, std::span<std::uint8_t> bytes,
                           std::chrono::milliseconds timeout) -> util::Result<>
    {
        if (__get_IPSR() || address > 0x7F || bytes.empty() || !util::valid_transfer_size(bytes.size()) ||
            !util::valid_transfer_timeout(timeout)) { return std::unexpected(std::make_error_code(std::errc::invalid_argument)); }
        return make_result(HAL_I2C_Mem_Read(&m_handle, address << 1, reg, I2C_MEMADD_SIZE_8BIT,
                                          bytes.data(), bytes.size(), timeout.count()));
    }
}
