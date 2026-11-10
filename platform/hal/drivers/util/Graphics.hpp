#pragma once
#include "hal/drivers/itf/IGraphics.hpp"
#include <array>
#include <limits>
namespace hal::util
{
    inline constexpr unsigned RGB565_BYTES{ sizeof(std::uint16_t) }, ARGB8888_BYTES{ sizeof(std::uint32_t) };
    inline constexpr unsigned CHANNEL_MAX{ 255 }, CHANNEL_ROUND{ 127 };
    inline constexpr unsigned ALPHA_SHIFT{ 24 }, RED_SHIFT{ 16 }, GREEN_SHIFT{ 8 };
    inline constexpr unsigned RGB565_RED_SHIFT{ 11 }, RGB565_GREEN_SHIFT{ 5 };
    inline constexpr unsigned RED_BLUE_MAX{ 31 }, GREEN_MAX{ 63 };
    inline constexpr unsigned RGB565_RED_MASK{ 0xf800 }, RGB565_GREEN_MASK{ 0x07e0 },
      RGB565_BLUE_MASK{ 0x001f };
    inline constexpr std::uint32_t OPAQUE_ALPHA{ 0xff000000U };
    inline constexpr unsigned DMA_MAX_WIDTH{ 0x3fff }, DMA_MAX_HEIGHT{ 0xffff };
    inline constexpr std::array CHANNEL_SHIFTS{ 0U, GREEN_SHIFT, RED_SHIFT };
    inline bool valid_graphics_transfer(const graphics::Transfer& transfer) noexcept
    {
        if (!transfer.width || !transfer.height || transfer.width > DMA_MAX_WIDTH ||
            transfer.height > DMA_MAX_HEIGHT) {
            return false;
        }
        const auto fits{ [&](std::size_t size, std::size_t stride, unsigned bytes) {
            const auto row{ static_cast<std::size_t>(transfer.width) * bytes };
            return stride >= row && stride % bytes == 0 &&
                   (stride / bytes) - transfer.width <= DMA_MAX_WIDTH &&
                   stride <= (std::numeric_limits<std::size_t>::max() - row) / transfer.height &&
                   size >= ((transfer.height - 1) * stride) + row;
        } };
        if (!transfer.destination.data() ||
            !fits(transfer.destination.size(), transfer.destination_stride, 2) ||
            // Inspect byte addresses for alignment/overlap without dereferencing a cast pointer.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
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
        if (transfer.source_format != graphics::Format::Rgb565 &&
            transfer.source_format != graphics::Format::Argb8888) {
            return false;
        }
        const unsigned bytes{ transfer.source_format == graphics::Format::Rgb565 ? 2U : 4U };
        // Inspect byte addresses for alignment/overlap without dereferencing a cast pointer.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto source{ reinterpret_cast<std::uintptr_t>(transfer.source.data()) };
        // Inspect byte addresses for alignment/overlap without dereferencing a cast pointer.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto destination{ reinterpret_cast<std::uintptr_t>(transfer.destination.data()) };
        const bool overlap{ source <= destination ? destination - source < transfer.source.size()
                                                  : source - destination < transfer.destination.size() };
        return transfer.source.data() && !overlap &&
               fits(transfer.source.size(), transfer.source_stride, bytes) &&
               // Inspect byte addresses for alignment/overlap without dereferencing a cast pointer.
               // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
               reinterpret_cast<std::uintptr_t>(transfer.source.data()) % bytes == 0;
    }
    inline std::uint16_t rgb565(std::uint32_t color) noexcept
    {
        return static_cast<std::uint16_t>(((color >> GREEN_SHIFT) & RGB565_RED_MASK) |
                                          ((color >> RGB565_GREEN_SHIFT) & RGB565_GREEN_MASK) |
                                          ((color >> 3) & RGB565_BLUE_MASK));
    }
    inline std::uint32_t argb8888(std::uint16_t color) noexcept
    {
        const auto r{ (color >> RGB565_RED_SHIFT) & RED_BLUE_MAX };
        const auto g{ (color >> RGB565_GREEN_SHIFT) & GREEN_MAX };
        const auto b{ color & RED_BLUE_MAX };
        return OPAQUE_ALPHA | ((r << 3 | r >> 2) << RED_SHIFT) | ((g << 2 | g >> 4) << GREEN_SHIFT) |
               (b << 3 | b >> 2);
    }
}
