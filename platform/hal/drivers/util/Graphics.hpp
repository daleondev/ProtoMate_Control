#pragma once
#include "hal/drivers/itf/IGraphics.hpp"
#include <limits>
namespace hal::util
{
    inline bool valid_graphics_transfer(const graphics::Transfer& transfer) noexcept
    {
        if (!transfer.width || !transfer.height || transfer.width > 0x3fff || transfer.height > 0xffff) {
            return false;
        }
        const auto fits = [&](std::size_t size, std::size_t stride, unsigned bytes) {
            const auto row = static_cast<std::size_t>(transfer.width) * bytes;
            return stride >= row && stride % bytes == 0 && stride / bytes - transfer.width <= 0x3fff &&
                   stride <= (std::numeric_limits<std::size_t>::max() - row) / transfer.height &&
                   size >= (transfer.height - 1) * stride + row;
        };
        if (!transfer.destination.data() ||
            !fits(transfer.destination.size(), transfer.destinationStride, 2) ||
            reinterpret_cast<std::uintptr_t>(transfer.destination.data()) % 2) {
            return false;
        }
        if (transfer.operation == graphics::Operation::Fill ||
            transfer.operation == graphics::Operation::BlendFill) {
            return true;
        }
        if (transfer.operation != graphics::Operation::Copy &&
            transfer.operation != graphics::Operation::Blend) {
            return false;
        }
        if (transfer.sourceFormat != graphics::Format::Rgb565 &&
            transfer.sourceFormat != graphics::Format::Argb8888) {
            return false;
        }
        const unsigned bytes = transfer.sourceFormat == graphics::Format::Rgb565 ? 2 : 4;
        const auto source = reinterpret_cast<std::uintptr_t>(transfer.source.data());
        const auto destination = reinterpret_cast<std::uintptr_t>(transfer.destination.data());
        const bool overlap = source <= destination ? destination - source < transfer.source.size()
                                                   : source - destination < transfer.destination.size();
        return transfer.source.data() && !overlap &&
               fits(transfer.source.size(), transfer.sourceStride, bytes) &&
               reinterpret_cast<std::uintptr_t>(transfer.source.data()) % bytes == 0;
    }
    inline std::uint16_t rgb565(std::uint32_t color) noexcept
    {
        return static_cast<std::uint16_t>(((color >> 8) & 0xf800) | ((color >> 5) & 0x7e0) |
                                          ((color >> 3) & 0x1f));
    }
    inline std::uint32_t argb8888(std::uint16_t color) noexcept
    {
        const auto r = (color >> 11) & 31, g = (color >> 5) & 63, b = color & 31;
        return 0xff000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
    }
}
