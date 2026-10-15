#include "Graphics.hpp"
#include "hal/drivers/util/Graphics.hpp"
#include <cstring>
namespace hal
{
    auto Graphics::submit(const graphics::Transfer& t, graphics::Callback callback) -> util::Result<>
    {
        if (!util::valid_graphics_transfer(t)) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        for (unsigned y = 0; y < t.height; ++y) {
            for (unsigned x = 0; x < t.width; ++x) {
                std::uint32_t foreground = t.color;
                const unsigned bytes = t.sourceFormat == graphics::Format::Rgb565 ? 2 : 4;
                if (t.operation != graphics::Operation::Fill &&
                    t.operation != graphics::Operation::BlendFill) {
                    if (bytes == 2) {
                        std::uint16_t value{};
                        std::memcpy(&value, t.source.data() + y * t.sourceStride + x * 2, 2);
                        foreground = util::argb8888(value);
                    }
                    else {
                        std::memcpy(&foreground, t.source.data() + y * t.sourceStride + x * 4, 4);
                    }
                }
                auto* destination = t.destination.data() + y * t.destinationStride + x * 2;
                if (t.operation == graphics::Operation::Blend ||
                    t.operation == graphics::Operation::BlendFill) {
                    std::uint16_t old{};
                    std::memcpy(&old, destination, 2);
                    const auto background = util::argb8888(old);
                    const auto alpha = ((foreground >> 24) * t.alpha + 127) / 255;
                    std::uint32_t combined{};
                    for (unsigned shift : { 0u, 8u, 16u }) {
                        combined |= (((((foreground >> shift) & 255) * alpha +
                                       ((background >> shift) & 255) * (255 - alpha)) +
                                      127) /
                                     255)
                                    << shift;
                    }
                    foreground = combined;
                }
                const auto pixel = util::rgb565(foreground);
                std::memcpy(destination, &pixel, 2);
            }
        }
        callback(graphics::Completion::Complete);
        return {};
    }
}
