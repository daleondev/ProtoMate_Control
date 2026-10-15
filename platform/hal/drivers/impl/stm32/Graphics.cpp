#include "Graphics.hpp"
#include "hal/drivers/util/Graphics.hpp"
#include "hal/stm32/InterruptGuard.hpp"
namespace
{
    hal::Graphics* instance{};
    bool accessible(std::uintptr_t address, std::size_t size, bool writable)
    {
        const auto within = [&](std::uintptr_t start, std::uintptr_t end) {
            return address >= start && address < end && size <= end - address;
        };
        return within(0x24000000, 0x24080000) || within(0x30000000, 0x30048000) ||
               within(0x38000000, 0x38010000) || (!writable && within(0x90000000, 0x91000000)) ||
               within(0xc0000000, 0xc0800000);
    }
    bool noncacheable(std::uintptr_t address, std::size_t size)
    {
        return (address >= 0xc0000000 && address < 0xc0600000 && size <= 0xc0600000 - address) ||
               (address >= 0x38000000 && address < 0x38010000 && size <= 0x38010000 - address);
    }
    void cache(std::span<const std::byte> bytes, bool invalidate)
    {
        const auto begin = reinterpret_cast<std::uintptr_t>(bytes.data());
        // The framebuffer range is non-cacheable; QSPI assets are immutable.
        if (noncacheable(begin, bytes.size()) || (begin >= 0x90000000 && begin < 0x91000000)) {
            return;
        }
        const auto aligned = begin & ~std::uintptr_t{ 31 };
        const auto length = (begin + bytes.size() + 31 - aligned) & ~std::uintptr_t{ 31 };
        if (invalidate) {
            SCB_InvalidateDCache_by_Addr(reinterpret_cast<std::uint32_t*>(aligned),
                                         static_cast<std::int32_t>(length));
        }
        else {
            SCB_CleanDCache_by_Addr(reinterpret_cast<std::uint32_t*>(aligned),
                                    static_cast<std::int32_t>(length));
        }
    }
}
extern "C" void DMA2D_IRQHandler()
{
    if (instance) {
        instance->interrupt();
    }
}
namespace hal
{
    Graphics::Graphics(DMA2D_TypeDef& registers)
      : m_registers(registers)
    {
        stm32::InterruptGuard guard;
        __HAL_RCC_DMA2D_CLK_ENABLE();
        instance = this;
        HAL_NVIC_SetPriority(DMA2D_IRQn, 11, 0);
        HAL_NVIC_EnableIRQ(DMA2D_IRQn);
    }
    Graphics::~Graphics()
    {
        abort();
        stm32::InterruptGuard guard;
        HAL_NVIC_DisableIRQ(DMA2D_IRQn);
        HAL_NVIC_ClearPendingIRQ(DMA2D_IRQn);
        if (instance == this) {
            instance = nullptr;
        }
    }
    bool Graphics::busy() const noexcept
    {
        stm32::InterruptGuard guard;
        return m_busy;
    }
    std::uint32_t Graphics::errors() const noexcept
    {
        stm32::InterruptGuard guard;
        return m_errors;
    }
    auto Graphics::submit(const graphics::Transfer& t, graphics::Callback callback) -> util::Result<>
    {
        const auto destination = reinterpret_cast<std::uintptr_t>(t.destination.data());
        if (!util::valid_graphics_transfer(t) || !accessible(destination, t.destination.size(), true) ||
            (!noncacheable(destination, t.destination.size()) &&
             (destination % 32 || t.destination.size() % 32)) ||
            (t.operation != graphics::Operation::Fill && t.operation != graphics::Operation::BlendFill &&
             !accessible(reinterpret_cast<std::uintptr_t>(t.source.data()), t.source.size(), false))) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        stm32::InterruptGuard guard;
        if (m_busy) {
            return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
        }
        cache(t.destination, false);
        if (t.operation != graphics::Operation::Fill && t.operation != graphics::Operation::BlendFill) {
            cache(t.source, false);
        }
        m_destination = t.destination;
        m_callback = callback;
        m_busy = true;
        auto& r = m_registers;
        r.CR = 0;
        r.IFCR = 0x3f;
        // Leave arbitration opportunities for LTDC's continuous SDRAM reads.
        // Program each submission too, since abort() resets this register.
        r.AMTCR = DMA2D_AMTCR_EN | (16u << DMA2D_AMTCR_DT_Pos);
        r.OPFCCR = DMA2D_OUTPUT_RGB565;
        r.OMAR = reinterpret_cast<std::uint32_t>(t.destination.data());
        r.OOR = t.destinationStride / 2 - t.width;
        r.NLR = (t.width << 16) | t.height;
        std::uint32_t mode{};
        if (t.operation == graphics::Operation::Fill) {
            mode = DMA2D_R2M;
            r.OCOLR = util::rgb565(t.color);
        }
        else if (t.operation == graphics::Operation::BlendFill) {
            mode = DMA2D_M2M_BLEND_FG;
            const auto alpha = (t.color >> 24) * t.alpha / 255;
            r.FGCOLR = t.color & 0xffffffu;
            r.FGPFCCR = DMA2D_INPUT_ARGB8888 | (DMA2D_REPLACE_ALPHA << 16) | (alpha << 24);
            r.BGMAR = r.OMAR;
            r.BGOR = r.OOR;
            r.BGPFCCR = DMA2D_INPUT_RGB565 | (DMA2D_REPLACE_ALPHA << 16) | (255u << 24);
        }
        else {
            const auto bytes = t.sourceFormat == graphics::Format::Rgb565 ? 2u : 4u;
            r.FGMAR = reinterpret_cast<std::uint32_t>(t.source.data());
            r.FGOR = t.sourceStride / bytes - t.width;
            r.FGPFCCR =
              t.sourceFormat == graphics::Format::Rgb565 ? DMA2D_INPUT_RGB565 : DMA2D_INPUT_ARGB8888;
            if (t.operation == graphics::Operation::Blend) {
                mode = DMA2D_M2M_BLEND;
                r.FGPFCCR |= (DMA2D_COMBINE_ALPHA << 16) | (std::uint32_t(t.alpha) << 24);
                r.BGMAR = r.OMAR;
                r.BGOR = r.OOR;
                r.BGPFCCR = DMA2D_INPUT_RGB565 | (DMA2D_REPLACE_ALPHA << 16) | (255u << 24);
            }
            else {
                mode = DMA2D_M2M_PFC;
            }
        }
        __DSB();
        r.CR = mode | DMA2D_CR_TCIE | DMA2D_CR_TEIE | DMA2D_CR_CEIE | DMA2D_CR_START;
        return {};
    }
    void Graphics::interrupt() noexcept
    {
        const auto flags = m_registers.ISR;
        m_registers.IFCR = flags;
        if (!m_busy || !(flags & (DMA2D_ISR_TCIF | DMA2D_ISR_TEIF | DMA2D_ISR_CEIF))) {
            return;
        }
        m_registers.CR = 0;
        const bool failed = flags & (DMA2D_ISR_TEIF | DMA2D_ISR_CEIF);
        if (failed) {
            ++m_errors;
        }
        cache(m_destination, true);
        m_busy = false;
        const auto callback = m_callback;
        m_callback = {};
        callback(failed ? graphics::Completion::Error : graphics::Completion::Complete);
    }
    void Graphics::abort() noexcept
    {
        stm32::InterruptGuard guard;
        if (m_busy) {
            // Reset synchronously terminates the bus master, even if an IRQ was lost.
            __HAL_RCC_DMA2D_FORCE_RESET();
            __DSB();
            __HAL_RCC_DMA2D_RELEASE_RESET();
            __DSB();
            cache(m_destination, true);
            m_busy = false;
            const auto callback = m_callback;
            m_callback = {};
            callback(graphics::Completion::Aborted);
        }
        m_registers.IFCR = 0x3f;
        HAL_NVIC_ClearPendingIRQ(DMA2D_IRQn);
    }
}
