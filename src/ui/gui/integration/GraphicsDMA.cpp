#include "GraphicsDMA.hpp"
#include "hal/drivers/util/Graphics.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <touchgfx/Bitmap.hpp>
namespace ui::gui
{
    GraphicsDMA::GraphicsDMA(std::shared_ptr<hal::IGraphics> graphics, std::stop_token stop)
      : DMA_Interface(m_queue)
      , m_queue(&m_storage, 1)
      , m_graphics(std::move(graphics))
      , m_stop(stop)
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
        const bool alphaFill = op.operation == BLIT_OP_FILL_WITH_ALPHA;
        const bool fill = (op.operation == BLIT_OP_FILL || op.operation == BLIT_OP_FILL_16BIT);
        const bool blend = alphaFill || op.operation == BLIT_OP_COPY_WITH_ALPHA ||
                           op.operation == BLIT_OP_COPY_ARGB8888 ||
                           op.operation == BLIT_OP_COPY_ARGB8888_WITH_ALPHA;
        // LCD16bpp can pack two RGB565 pixels into an ARGB8888-width
        // raw fill/copy. Normalize those word operations to our RGB565 HAL.
        const unsigned packed = op.dstFormat == Bitmap::ARGB8888 ? 2u : 1u;
        if (packed == 2 && (blend || (!fill && op.operation != BLIT_OP_COPY))) {
            m_failed = true;
            return;
        }
        const unsigned sourceBytes = packed == 2 ? 2 : op.srcFormat == Bitmap::ARGB8888 ? 4 : 2;
        const auto width = unsigned(op.nSteps) * packed;
        const auto destinationStride = std::size_t(op.dstLoopStride) * packed * 2;
        const auto sourceStride = std::size_t(op.srcLoopStride) * packed * sourceBytes;
        // Despite BlitOp.hpp's legacy comment, these strides are in pixels,
        // as used by the SDK's generated STM32 DMA adapter.
        hal::graphics::Transfer transfer{
            .operation = alphaFill ? hal::graphics::Operation::BlendFill
                         : fill    ? hal::graphics::Operation::Fill
                         : blend   ? hal::graphics::Operation::Blend
                                   : hal::graphics::Operation::Copy,
            .destination = { reinterpret_cast<std::byte*>(op.pDst),
                             (std::size_t(op.nLoops) - 1) * destinationStride + width * 2 },
            .destinationStride = destinationStride,
            .source = {},
            .sourceStride = sourceStride,
            .sourceFormat =
              sourceBytes == 4 ? hal::graphics::Format::Argb8888 : hal::graphics::Format::Rgb565,
            .width = width,
            .height = op.nLoops,
            .color = alphaFill ? (0xff000000u | op.color)
                     : fill    ? hal::util::argb8888(static_cast<std::uint16_t>(op.color))
                               : static_cast<std::uint32_t>(op.color),
            .alpha = op.alpha
        };
        if (!fill && !alphaFill) {
            transfer.source = { reinterpret_cast<const std::byte*>(op.pSrc),
                                (std::size_t(op.nLoops) - 1) * sourceStride + width * sourceBytes };
        }
        const auto began=std::chrono::steady_clock::now();
        if (!execute(transfer)) {
            m_failed = true;
        }
        if (!fill && !alphaFill) {
            m_imageTransferUs+=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-began).count();
            ++m_imageTransfers;
        }
    }
    bool GraphicsDMA::execute(const hal::graphics::Transfer& transfer)
    {
        m_done.store(false, std::memory_order_release);
        const std::stop_callback cancellation(m_stop, [this] { m_completion.signal(); });
        const auto result =
          m_graphics->submit(transfer, { [](void* pointer, hal::graphics::Completion completion) noexcept {
            auto& self = *static_cast<GraphicsDMA*>(pointer);
            self.m_result.store(completion, std::memory_order_relaxed);
            self.m_done.store(true, std::memory_order_release);
            self.m_completion.signal();
        }, this });
        if (!result) {
            return false;
        }
        // One in-flight operation. Suspend the GUI task while DMA2D runs;
        // no widget/engine code runs in the IRQ, and no per-operation allocation.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{ 100 };
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
        const auto offset = std::size_t(region.y) * 1024 + region.x;
        const auto length = std::size_t(region.height - 1) * 1024 + region.width;
        return execute({ .operation = hal::graphics::Operation::Copy,
                         .destination = std::as_writable_bytes(destination.subspan(offset, length)),
                         .destinationStride = 2048,
                         .source = std::as_bytes(source.subspan(offset, length)),
                         .sourceStride = 2048,
                         .width = unsigned(region.width),
                         .height = unsigned(region.height) });
    }
    bool GraphicsDMA::verify(std::span<std::uint16_t> scratch)
    {
        if (!m_graphics || scratch.size() < 8192) {
            return false;
        }
        using namespace hal::graphics;
        constexpr unsigned width = 33, height = 17, stride = 79;
        auto destination = scratch.subspan(1, stride * height);
        auto source = std::as_writable_bytes(scratch.subspan(4096, 2048));
        std::ranges::fill(scratch.first(4096), 0x1234);
        Transfer transfer{ .operation = Operation::Fill,
                           .destination = std::as_writable_bytes(destination),
                           .destinationStride = stride * 2,
                           .source = {},
                           .width = width,
                           .height = height,
                           .color = 0xff31ad73 };
        if (!execute(transfer)) {
            return false;
        }
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < stride; ++x) {
                if (destination[y * stride + x] != (x < width ? hal::util::rgb565(transfer.color) : 0x1234)) {
                    return false;
                }
            }
        }
        if (scratch[0] != 0x1234 || scratch[1 + stride * height] != 0x1234) {
            return false;
        }
        // Solid translucent fills must use the fixed-foreground DMA path too.
        // CPU read/modify/write blending can starve LTDC's SDRAM read FIFO.
        const auto background = hal::util::argb8888(hal::util::rgb565(transfer.color));
        transfer.operation = Operation::BlendFill;
        transfer.color = 0xffedf4f6;
        transfer.alpha = 80;
        if (!execute(transfer)) {
            return false;
        }
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                const auto actual = hal::util::argb8888(destination[y * stride + x]);
                for (unsigned shift : { 0u, 8u, 16u }) {
                    const int expected =
                      (((transfer.color >> shift) & 255) * 80 + ((background >> shift) & 255) * 175) / 255;
                    const int error = int((actual >> shift) & 255) - expected;
                    if (error < -8 || error > 8) {
                        return false;
                    }
                }
            }
        }
        for (unsigned i = 0; i < width * height; ++i) {
            const std::uint32_t pixel =
              (i % 256) << 24 | ((i * 7) % 256) << 16 | ((i * 11) % 256) << 8 | ((i * 13) % 256);
            std::memcpy(source.data() + i * 4, &pixel, 4);
        }
        transfer.operation = Operation::Copy;
        transfer.source = source;
        transfer.sourceStride = width * 4;
        transfer.sourceFormat = Format::Argb8888;
        if (!execute(transfer)) {
            return false;
        }
        std::array<std::uint16_t, width * height> before{};
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                std::uint32_t pixel{};
                std::memcpy(&pixel, source.data() + (y * width + x) * 4, 4);
                if (destination[y * stride + x] != hal::util::rgb565(pixel)) {
                    return false;
                }
                before[y * width + x] = destination[y * stride + x];
            }
        }
        // Offset source values exercise per-pixel AND constant alpha.
        for (unsigned i = 0; i < width * height; ++i) {
            const std::uint32_t pixel = 0x809b47e3;
            std::memcpy(source.data() + i * 4, &pixel, 4);
        }
        transfer.operation = Operation::Blend;
        transfer.alpha = 173;
        if (!execute(transfer)) {
            return false;
        }
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                const auto background = hal::util::argb8888(before[y * width + x]);
                const auto actual = hal::util::argb8888(destination[y * stride + x]);
                constexpr unsigned alpha = 128 * 173 / 255;
                for (unsigned shift : { 0u, 8u, 16u }) {
                    const int expected = ((((0x9b47e3u >> shift) & 255) * alpha +
                                           ((background >> shift) & 255) * (255 - alpha)) /
                                          255);
                    const int error = int((actual >> shift) & 255) - expected;
                    if (error < -8 || error > 8) {
                        return false;
                    } // one RGB565 quantization step
                }
            }
        }
        transfer.operation = Operation::Copy;
        transfer.source = std::as_bytes(scratch.subspan(4096, 2048));
        transfer.sourceFormat = Format::Rgb565;
        transfer.sourceStride = width * 2;
        if (!execute(transfer)) {
            return false;
        }
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                if (destination[y * stride + x] != scratch[4096 + y * width + x]) {
                    return false;
                }
            }
        }
        return !m_graphics->busy() && !m_graphics->errors();
    }
}
