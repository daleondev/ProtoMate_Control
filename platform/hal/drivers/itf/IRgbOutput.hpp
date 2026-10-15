#pragma once
#include "DisplayEvents.hpp"
#include "hal/util/Result.hpp"
#include <cstdint>
#include <span>
namespace hal
{
    class IRgbOutput
    {
      public:
        virtual ~IRgbOutput() = default;
        // Two RGB565 framebuffers; only render to the inactive buffer.
        virtual auto initialize() -> util::Result<> = 0;
        virtual auto framebuffer(unsigned slot) -> std::span<std::uint16_t> = 0;
        virtual auto present(unsigned slot) -> util::Result<> = 0;
        // One pending frame. Ownership returns only after Presented or shutdown.
        virtual auto requestPresent(unsigned slot) -> util::Result<> = 0;
        virtual auto state() const noexcept -> display::State = 0;
        // Callback may run in an ISR. Removal waits out any invocation.
        virtual void setCallback(display::Callback callback) noexcept = 0;
        virtual void shutdown() noexcept = 0;
        // Native host mailboxes are drained by a ThreadX worker, never by SDL.
        virtual void service() noexcept {}
        virtual auto servicePeriod() const noexcept -> std::chrono::milliseconds
        {
            return std::chrono::milliseconds{ 50 };
        }
        virtual auto fill(unsigned slot, std::uint16_t rgb565) -> util::Result<> = 0;
        virtual auto backlight(unsigned percent) -> util::Result<> = 0;
        virtual auto underruns() const noexcept -> std::uint32_t = 0;
        virtual auto width() const noexcept -> unsigned = 0;
        virtual auto height() const noexcept -> unsigned = 0;
    };
}
