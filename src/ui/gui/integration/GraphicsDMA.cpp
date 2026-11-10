#include "GraphicsDMA.hpp"
#include "hal/drivers/util/Graphics.hpp"
#include "profiling/Trace.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <touchgfx/Bitmap.hpp>
#include <utility>
namespace ui::gui
{
    namespace
    {
        constexpr auto TRANSFER_TIMEOUT{ std::chrono::milliseconds{ 100 } };
        constexpr std::size_t DISPLAY_WIDTH{ 1024 }, DISPLAY_STRIDE{ DISPLAY_WIDTH * sizeof(std::uint16_t) };
        constexpr std::size_t VERIFY_SCRATCH{ 8192 }, VERIFY_SOURCE_START{ 4096 }, VERIFY_SOURCE_SIZE{ 2048 };
        constexpr std::uint16_t VERIFY_GUARD{ 0x1234 };
        constexpr std::uint32_t VERIFY_FILL{ 0xff31ad73 }, VERIFY_BLEND_FILL{ 0xffedf4f6 },
          VERIFY_BLEND_PIXEL{ 0x809b47e3 };
        constexpr unsigned VERIFY_FILL_ALPHA{ 80 }, VERIFY_BLEND_ALPHA{ 173 }, VERIFY_PIXEL_ALPHA{ 128 };
        constexpr unsigned VERIFY_RED_FACTOR{ 7 }, VERIFY_GREEN_FACTOR{ 11 }, VERIFY_BLUE_FACTOR{ 13 };
        constexpr int QUANTIZATION_MARGIN{ 8 };
        hal::graphics::Operation operation_for(std::uint32_t operation)
        {
            using namespace touchgfx;
            using enum hal::graphics::Operation;
            switch (operation) {
                case BLIT_OP_FILL_WITH_ALPHA:
                    return BlendFill;
                case BLIT_OP_FILL:
                case BLIT_OP_FILL_16BIT:
                    return Fill;
                case BLIT_OP_COPY_WITH_ALPHA:
                case BLIT_OP_COPY_ARGB8888:
                case BLIT_OP_COPY_ARGB8888_WITH_ALPHA:
                    return Blend;
                default:
                    return Copy;
            }
        }
        std::uint32_t fill_color(std::uint32_t color, hal::graphics::Operation operation)
        {
            if (operation == hal::graphics::Operation::BlendFill) {
                return hal::util::OPAQUE_ALPHA | color;
            }
            if (operation == hal::graphics::Operation::Fill) {
                return hal::util::argb8888(static_cast<std::uint16_t>(color));
            }
            return color;
        }
        struct PixelRegion
        {
            unsigned width, height, stride;
        };
        struct BlendExpectation
        {
            std::uint32_t foreground, background;
            unsigned alpha;
        };
        struct Foreground
        {
            std::uint32_t color;
            unsigned alpha;
        };
        template<class Expected>
        bool pixels_match(std::span<const std::uint16_t> pixels, PixelRegion region, Expected expected)
        {
            for (unsigned y{ 0 }; y < region.height; ++y) {
                for (unsigned x{ 0 }; x < region.width; ++x) {
                    if (pixels[(std::size_t(y) * region.stride) + x] != expected(x, y)) {
                        return false;
                    }
                }
            }
            return true;
        }
        bool blend_matches(std::uint32_t actual, BlendExpectation expected_blend)
        {
            using namespace hal::util;
            return std::ranges::all_of(CHANNEL_SHIFTS, [&](unsigned shift) {
                const int expected{ static_cast<int>(
                  (((expected_blend.foreground >> shift) & CHANNEL_MAX) * expected_blend.alpha +
                   ((expected_blend.background >> shift) & CHANNEL_MAX) *
                     (CHANNEL_MAX - expected_blend.alpha)) /
                  CHANNEL_MAX) };
                const int error{ int((actual >> shift) & CHANNEL_MAX) - expected };
                return error >= -QUANTIZATION_MARGIN && error <= QUANTIZATION_MARGIN;
            });
        }
    }
    namespace
    {
        template<class Background>
        bool verify_blend(std::span<const std::uint16_t> pixels,
                          PixelRegion region,
                          Foreground foreground,
                          Background background)
        {
            for (unsigned y{ 0 }; y < region.height; ++y) {
                for (unsigned x{ 0 }; x < region.width; ++x) {
                    const auto actual{ hal::util::argb8888(pixels[(std::size_t(y) * region.stride) + x]) };
                    if (!blend_matches(actual,
                                       { .foreground = foreground.color,
                                         .background = background(x, y),
                                         .alpha = foreground.alpha })) {
                        return false;
                    }
                }
            }
            return true;
        }
    }
    GraphicsDMA::GraphicsDMA(std::shared_ptr<hal::IGraphics> graphics, std::stop_token stop)
      : DMA_Interface{ m_queue }
      , m_queue{ &m_storage, 1 }
      , m_graphics{ std::move(graphics) }
      , m_stop{ std::move(stop) }
    {
    }
    GraphicsDMA::~GraphicsDMA()
    {
        if (m_graphics) {
            m_graphics->abort();
        }
    }
    touchgfx::BlitOperations GraphicsDMA::getBlitCaps()
    {
        using namespace touchgfx;
        // The TouchGFX ABI defines this enum as a bitmask, including zero for
        // software-only rendering. Its representable range is 0..4095; these
        // combinations are intentional, even though they are not named enumerators.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        return static_cast<BlitOperations>(m_graphics
                                             ? BLIT_OP_FILL | BLIT_OP_FILL_16BIT | BLIT_OP_FILL_WITH_ALPHA |
                                                 BLIT_OP_COPY | BLIT_OP_COPY_16BIT | BLIT_OP_COPY_WITH_ALPHA |
                                                 BLIT_OP_COPY_ARGB8888 | BLIT_OP_COPY_ARGB8888_WITH_ALPHA
                                             : 0);
    }
    void GraphicsDMA::addToQueue(const touchgfx::BlitOp& op)
    {
        using namespace touchgfx;
        if (m_failed || m_stop.stop_requested()) {
            return;
        }
        if (!op.nSteps || !op.nLoops) {
            return;
        }
        const bool alpha_fill{ op.operation == BLIT_OP_FILL_WITH_ALPHA };
        const bool fill{ (op.operation == BLIT_OP_FILL || op.operation == BLIT_OP_FILL_16BIT) };
        const bool blend{ alpha_fill || op.operation == BLIT_OP_COPY_WITH_ALPHA ||
                          op.operation == BLIT_OP_COPY_ARGB8888 ||
                          op.operation == BLIT_OP_COPY_ARGB8888_WITH_ALPHA };
        // LCD16bpp can pack two RGB565 pixels into an ARGB8888-width
        // raw fill/copy. Normalize those word operations to our RGB565 HAL.
        const unsigned packed{ op.dstFormat == Bitmap::ARGB8888 ? 2U : 1U };
        if (packed == 2 && (blend || (!fill && op.operation != BLIT_OP_COPY))) {
            m_failed = true;
            return;
        }
        const unsigned source_bytes{ packed != 2 && op.srcFormat == Bitmap::ARGB8888 ? 4U : 2U };
        const auto width{ unsigned(op.nSteps) * packed };
        const auto destination_stride{ std::size_t(op.dstLoopStride) * packed * 2 };
        const auto source_stride{ std::size_t(op.srcLoopStride) * packed * source_bytes };
        // Despite BlitOp.hpp's legacy comment, these strides are in pixels,
        // as used by the SDK's generated STM32 DMA adapter.
        hal::graphics::Transfer transfer{
            .operation = operation_for(op.operation),
            .destination = std::as_writable_bytes(
              std::span{ op.pDst,
                         (((std::size_t(op.nLoops) - 1) * destination_stride) + (std::size_t(width) * 2)) /
                           sizeof(*op.pDst) }),
            .destination_stride = destination_stride,
            .source = {},
            .source_stride = source_stride,
            .source_format =
              source_bytes == 4 ? hal::graphics::Format::Argb8888 : hal::graphics::Format::Rgb565,
            .width = width,
            .height = op.nLoops,
            .color = fill_color(op.color, operation_for(op.operation)),
            .alpha = op.alpha
        };
        if (!fill && !alpha_fill) {
            transfer.source = std::as_bytes(std::span{
              op.pSrc,
              (((std::size_t(op.nLoops) - 1) * source_stride) + (std::size_t(width) * source_bytes)) /
                sizeof(*op.pSrc) });
        }
        const auto began{ std::chrono::steady_clock::now() };
        if (!performTransfer(transfer)) {
            m_failed = true;
        }
        if (!fill && !alpha_fill) {
            m_imageTransferUs +=
              std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began)
                .count();
            ++m_imageTransfers;
        }
    }
    bool GraphicsDMA::performTransfer(const hal::graphics::Transfer& transfer)
    {
        profiling::Scope measurement{ PROFILE_SITE_GUI_DMA, PROFILE_CAT_GRAPHICS };
        m_done.store(false, std::memory_order_release);
        const std::stop_callback cancellation{ m_stop, [this] { m_completion.signal(); } };
        const auto result{ m_graphics->submit(
          transfer, { .function = [](void* pointer, hal::graphics::Completion completion) noexcept {
            auto& self{ *static_cast<GraphicsDMA*>(pointer) };
            self.m_result.store(completion, std::memory_order_relaxed);
            self.m_done.store(true, std::memory_order_release);
            self.m_completion.signal();
        }, .context = this }) };
        if (!result) {
            return false;
        }
        // One in-flight operation. Suspend the GUI task while DMA2D runs;
        // no widget/engine code runs in the IRQ, and no per-operation allocation.
        const auto deadline{ std::chrono::steady_clock::now() + TRANSFER_TIMEOUT };
        while (!m_done.load(std::memory_order_acquire) && !m_stop.stop_requested()) {
            if (!m_completion.waitUntil(deadline)) {
                break;
            }
        }
        if (!m_done.load(std::memory_order_acquire) ||
            m_result.load() != hal::graphics::Completion::Complete) {
            m_graphics->abort();
            return false;
        }
        return true;
    }
    bool GraphicsDMA::copyRegion(std::span<const std::uint16_t> source,
                                 std::span<std::uint16_t> destination,
                                 const touchgfx::Rect& region)
    {
        const auto offset{ (std::size_t(region.y) * DISPLAY_WIDTH) + region.x };
        const auto length{ (std::size_t(region.height - 1) * DISPLAY_WIDTH) + region.width };
        return performTransfer({ .operation = hal::graphics::Operation::Copy,
                                 .destination = std::as_writable_bytes(destination.subspan(offset, length)),
                                 .destination_stride = DISPLAY_STRIDE,
                                 .source = std::as_bytes(source.subspan(offset, length)),
                                 .source_stride = DISPLAY_STRIDE,
                                 .width = unsigned(region.width),
                                 .height = unsigned(region.height) });
    }
    bool GraphicsDMA::verify(std::span<std::uint16_t> scratch)
    {
        if (!m_graphics || scratch.size() < VERIFY_SCRATCH) {
            return false;
        }
        using namespace hal::graphics;
        constexpr unsigned width{ 33 };
        constexpr unsigned height{ 17 };
        constexpr unsigned stride{ 79 };
        auto destination{ scratch.subspan(1, static_cast<std::size_t>(stride) * height) };
        auto source{ std::as_writable_bytes(scratch.subspan(VERIFY_SOURCE_START, VERIFY_SOURCE_SIZE)) };
        std::ranges::fill(scratch.first(VERIFY_SOURCE_START), VERIFY_GUARD);
        Transfer transfer{ .operation = Operation::Fill,
                           .destination = std::as_writable_bytes(destination),
                           .destination_stride = static_cast<std::size_t>(stride) * 2,
                           .source = {},
                           .width = width,
                           .height = height,
                           .color = VERIFY_FILL };
        if (!performTransfer(transfer)) {
            return false;
        }
        if (!pixels_match(destination,
                          { .width = stride, .height = height, .stride = stride },
                          [&](unsigned x, unsigned /*y*/) {
            return x < width ? hal::util::rgb565(transfer.color) : VERIFY_GUARD;
        })) {
            return false;
        }
        if (scratch[0] != VERIFY_GUARD || scratch[1 + (stride * height)] != VERIFY_GUARD) {
            return false;
        }
        // Solid translucent fills must use the fixed-foreground DMA path too.
        // CPU read/modify/write blending can starve LTDC's SDRAM read FIFO.
        const auto background{ hal::util::argb8888(hal::util::rgb565(transfer.color)) };
        transfer.operation = Operation::BlendFill;
        transfer.color = VERIFY_BLEND_FILL;
        transfer.alpha = VERIFY_FILL_ALPHA;
        if (!performTransfer(transfer)) {
            return false;
        }
        if (!verify_blend(destination,
                          { .width = width, .height = height, .stride = stride },
                          { .color = transfer.color, .alpha = transfer.alpha },
                          [&](unsigned /*x*/, unsigned /*y*/) { return background; })) {
            return false;
        }
        for (unsigned i{ 0 }; i < width * height; ++i) {
            const std::uint32_t pixel{ (i % (hal::util::CHANNEL_MAX + 1)) << hal::util::ALPHA_SHIFT |
                                       ((i * VERIFY_RED_FACTOR) % (hal::util::CHANNEL_MAX + 1))
                                         << hal::util::RED_SHIFT |
                                       ((i * VERIFY_GREEN_FACTOR) % (hal::util::CHANNEL_MAX + 1))
                                         << hal::util::GREEN_SHIFT |
                                       ((i * VERIFY_BLUE_FACTOR) % (hal::util::CHANNEL_MAX + 1)) };
            std::memcpy(source.data() + (static_cast<std::size_t>(i) * 4), &pixel, 4);
        }
        transfer.operation = Operation::Copy;
        transfer.source = source;
        transfer.source_stride = static_cast<std::size_t>(width) * 4;
        transfer.source_format = Format::Argb8888;
        if (!performTransfer(transfer)) {
            return false;
        }
        std::array<std::uint16_t, static_cast<std::size_t>(width) * height> before{};
        for (unsigned y{ 0 }; y < height; ++y) {
            for (unsigned x{ 0 }; x < width; ++x) {
                std::uint32_t pixel{};
                std::memcpy(&pixel, source.data() + ((std::size_t(y) * width + x) * sizeof(pixel)), 4);
                if (destination[(y * stride) + x] != hal::util::rgb565(pixel)) {
                    return false;
                }
                before[(y * width) + x] = destination[(y * stride) + x];
            }
        }
        // Offset source values exercise per-pixel AND constant alpha.
        for (unsigned i{ 0 }; i < width * height; ++i) {
            const std::uint32_t pixel{ VERIFY_BLEND_PIXEL };
            std::memcpy(source.data() + (static_cast<std::size_t>(i) * 4), &pixel, 4);
        }
        transfer.operation = Operation::Blend;
        transfer.alpha = VERIFY_BLEND_ALPHA;
        if (!performTransfer(transfer)) {
            return false;
        }
        constexpr unsigned alpha{ VERIFY_PIXEL_ALPHA * VERIFY_BLEND_ALPHA / hal::util::CHANNEL_MAX };
        if (!verify_blend(destination,
                          { .width = width, .height = height, .stride = stride },
                          { .color = VERIFY_BLEND_PIXEL, .alpha = alpha },
                          [&](unsigned x, unsigned y) {
            return hal::util::argb8888(before[(std::size_t(y) * width) + x]);
        })) {
            return false;
        }
        transfer.operation = Operation::Copy;
        transfer.source = std::as_bytes(scratch.subspan(VERIFY_SOURCE_START, VERIFY_SOURCE_SIZE));
        transfer.source_format = Format::Rgb565;
        transfer.source_stride = static_cast<std::size_t>(width) * 2;
        if (!performTransfer(transfer)) {
            return false;
        }
        if (!pixels_match(destination,
                          { .width = width, .height = height, .stride = stride },
                          [&](unsigned x, unsigned y) {
            return scratch[VERIFY_SOURCE_START + (std::size_t(y) * width) + x];
        })) {
            return false;
        }
        return !m_graphics->busy() && !m_graphics->errors();
    }
}
