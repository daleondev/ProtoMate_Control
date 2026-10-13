#pragma once
#include "hal/util/Result.hpp"
#include <array>
#include <cstdint>
namespace hal::device {
    struct TouchPoint { bool active{}; std::uint16_t x{}, y{}; };
    struct TouchReport { std::uint8_t firmware{}, status{}; std::array<TouchPoint,5> points{}; };
    class ITouch {
    public:
        virtual ~ITouch() = default;
        // Bounded I2C operations; absent panel returns an error without stopping UI.
        virtual auto initialize() -> util::Result<> = 0;
        virtual auto read() -> util::Result<TouchReport> = 0;
    };
}
