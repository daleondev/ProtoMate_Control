#pragma once
#include "hal/util/Result.hpp"
#include <array>
#include <chrono>
#include <cstdint>
namespace hal::device
{
    inline constexpr std::size_t MAX_TOUCH_POINTS{ 5 };
    struct TouchPoint
    {
        bool active{};
        std::uint16_t x{}, y{};
    };
    struct TouchReport
    {
        std::uint8_t firmware{}, status{};
        std::array<TouchPoint, MAX_TOUCH_POINTS> points{};
        bool cancelled{};
        std::uint32_t dropped_reports{};
        float wheel{}; // Relative desktop input, consumed once; zero on touch hardware.
        std::uint16_t pointer_x{}, pointer_y{};
    };
    class ITouch
    {
      public:
        ITouch() = default;
        ITouch(const ITouch&) = delete;
        auto operator=(const ITouch&) -> ITouch& = delete;
        ITouch(ITouch&&) = delete;
        auto operator=(ITouch&&) -> ITouch& = delete;

        using Callback = void (*)(void*) noexcept;
        virtual ~ITouch() = default;
        // Bounded I2C operations; absent panel returns an error without stopping UI.
        virtual auto initialize() -> util::Result<> = 0;
        virtual auto read() -> util::Result<TouchReport> = 0;
        // Notification only; acquiring coordinates belongs to a worker.
        virtual void setCallback(Callback callback, void* context) noexcept = 0;
        // IRQ-driven devices use a slow idle health check; pointer providers poll.
        virtual auto idlePeriod() const noexcept -> std::chrono::milliseconds
        {
            return std::chrono::seconds{ 1 };
        }
    };
}
