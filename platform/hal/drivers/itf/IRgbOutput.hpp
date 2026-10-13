#pragma once
#include "hal/util/Result.hpp"
#include <cstdint>
#include <span>
namespace hal {
    class IRgbOutput {
    public:
        virtual ~IRgbOutput() = default;
        // Two RGB565 framebuffers; only render to the inactive buffer.
        virtual auto initialize() -> util::Result<> = 0;
        virtual auto framebuffer(unsigned slot) -> std::span<std::uint16_t> = 0;
        virtual auto present(unsigned slot) -> util::Result<> = 0;
        virtual auto fill(unsigned slot, std::uint16_t rgb565) -> util::Result<> = 0;
        virtual auto backlight(unsigned percent) -> util::Result<> = 0;
        virtual auto underruns() const noexcept -> std::uint32_t = 0;
        virtual auto width() const noexcept -> unsigned = 0;
        virtual auto height() const noexcept -> unsigned = 0;
    };
}
