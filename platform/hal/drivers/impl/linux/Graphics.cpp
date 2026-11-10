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
        using namespace util;
        for (unsigned y{ 0 }; y < t.height; ++y) {
            for (unsigned x{ 0 }; x < t.width; ++x) {
                std::uint32_t foreground{ t.color };
                const unsigned bytes{ t.source_format == graphics::Format::Rgb565 ? 2U : 4U };
                if (t.operation != graphics::Operation::Fill &&
                    t.operation != graphics::Operation::BlendFill) {
                    if (bytes == 2) {
                        std::uint16_t value{};
                        std::memcpy(&value,
                                    t.source.data() + (std::size_t(y) * t.source_stride) +
                                      (static_cast<std::size_t>(x) * 2),
                                    2);
                        foreground = util::argb8888(value);
                    }
                    else {
                        std::memcpy(&foreground,
                                    t.source.data() + (std::size_t(y) * t.source_stride) +
                                      (static_cast<std::size_t>(x) * 4),
                                    4);
                    }
                }
                auto* destination{ t.destination.data() + (std::size_t(y) * t.destination_stride) +
                                   (static_cast<std::size_t>(x) * 2) };
                if (t.operation == graphics::Operation::Blend ||
                    t.operation == graphics::Operation::BlendFill) {
                    std::uint16_t old{};
                    std::memcpy(&old, destination, 2);
                    const auto background{ util::argb8888(old) };
                    const auto alpha{ ((foreground >> ALPHA_SHIFT) * t.alpha + CHANNEL_ROUND) / CHANNEL_MAX };
                    std::uint32_t combined{};
                    for (unsigned shift : CHANNEL_SHIFTS) {
                        combined |= (((((foreground >> shift) & CHANNEL_MAX) * alpha +
                                       ((background >> shift) & CHANNEL_MAX) * (CHANNEL_MAX - alpha)) +
                                      CHANNEL_ROUND) /
                                     CHANNEL_MAX)
                                    << shift;
                    }
                    foreground = combined;
                }
                const auto pixel{ util::rgb565(foreground) };
                std::memcpy(destination, &pixel, 2);
            }
        }
        callback(graphics::Completion::Complete);
        return {};
    }
}
