#pragma once
#include "hal/util/Result.hpp"
#include <chrono>
#include <cstdint>
#include <span>
namespace hal {
    class II2c {
    public:
        virtual ~II2c() = default;
        virtual auto probe(std::uint8_t address, std::chrono::milliseconds timeout) -> util::Result<> = 0;
        virtual auto readRegister(std::uint8_t address, std::uint8_t reg, std::span<std::uint8_t> bytes,
                                  std::chrono::milliseconds timeout) -> util::Result<> = 0;
    };
}
namespace hal::i2c { enum class Peripheral { I2c1, I2c2, I2c3, I2c4 }; }
