#pragma once
#include "hal/util/Result.hpp"
#include <array>
#include <cstdint>
#include <chrono>
namespace hal::device {
    struct TouchPoint { bool active{}; std::uint16_t x{}, y{}; };
    struct TouchReport { std::uint8_t firmware{}, status{}; std::array<TouchPoint,5> points{}; bool cancelled{}; std::uint32_t droppedReports{}; };
    class ITouch {
    public:
        using Callback = void (*)(void*) noexcept;
        virtual ~ITouch() = default;
        // Bounded I2C operations; absent panel returns an error without stopping UI.
        virtual auto initialize() -> util::Result<> = 0;
        virtual auto read() -> util::Result<TouchReport> = 0;
        // Notification only; acquiring coordinates belongs to a worker.
        virtual void setCallback(Callback callback, void* context) noexcept = 0;
        // IRQ-driven devices use a slow idle health check; pointer providers poll.
        virtual auto idlePeriod() const noexcept -> std::chrono::milliseconds { return std::chrono::milliseconds{1000}; }
    };
}
