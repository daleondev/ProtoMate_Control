#pragma once
#include "hal/drivers/itf/II2c.hpp"
#include "stm32h7xx_hal.h"
namespace hal {
    class I2c final : public II2c {
    public:
        explicit I2c(I2C_HandleTypeDef& handle) : m_handle(handle) {}
        auto probe(std::uint8_t address, std::chrono::milliseconds timeout) -> util::Result<> override;
        auto readRegister(std::uint8_t address, std::uint8_t reg, std::span<std::uint8_t> bytes,
                          std::chrono::milliseconds timeout) -> util::Result<> override;
    private:
        I2C_HandleTypeDef& m_handle;
    };
}
