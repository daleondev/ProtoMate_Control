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
        // HAL's memory-read entry otherwise spins for a fixed 25 ms, even
        // when the caller requested a shorter timeout. An absent/unpowered
        // peripheral must not monopolize this higher-priority input worker.
        if (__HAL_I2C_GET_FLAG(&m_handle, I2C_FLAG_BUSY)) {
            return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
        }
        return make_result(HAL_I2C_Mem_Read(&m_handle, address << 1, reg, I2C_MEMADD_SIZE_8BIT,
                                          bytes.data(), bytes.size(), timeout.count()));
    }
}
