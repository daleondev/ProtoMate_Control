// CMSIS/HAL register macros use fixed MMIO addresses and C pointer casts.
// NOLINTBEGIN(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr,cppcoreguidelines-avoid-do-while)
#include "Graphics.hpp"
#include "hal/drivers/util/Graphics.hpp"
#include "hal/stm32/InterruptGuard.hpp"
#include "profiling/Trace.hpp"
#include <algorithm>
#include <array>
namespace
{
    // IRQ dispatch pointer; installed/removed under the STM32 interrupt guard.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    hal::Graphics* instance{};
    struct Region
    {
        std::uintptr_t begin{}, end{};
        constexpr bool contains(std::uintptr_t address, std::size_t size) const noexcept
        {
            return address >= begin && address < end && size <= end - address;
        }
    };
    constexpr std::array RAM_REGIONS{ Region{ .begin = 0x24000000, .end = 0x24080000 },
                                      Region{ .begin = 0x30000000, .end = 0x30048000 },
                                      Region{ .begin = 0x38000000, .end = 0x38010000 },
                                      Region{ .begin = 0xc0000000, .end = 0xc0800000 } };
    constexpr Region QSPI{ .begin = 0x90000000, .end = 0x91000000 };
    constexpr Region UNCACHED_SDRAM{ .begin = 0xc0000000, .end = 0xc0600000 };
    constexpr Region SHARED_RAM{ .begin = 0x38000000, .end = 0x38010000 };
    constexpr std::uintptr_t CACHE_LINE_BYTES{ 32 };
    constexpr auto CACHE_LINE_MASK{ CACHE_LINE_BYTES - 1 };
    constexpr unsigned INTERRUPT_PRIORITY{ 11 };
    constexpr unsigned BUS_DEAD_TIME{ 16 };
    constexpr std::uint32_t ALPHA_MAX{ 255 };
    constexpr unsigned ARGB_ALPHA_SHIFT{ 24 };
    constexpr std::uint32_t RGB_MASK{ 0xffffff };
    constexpr std::uint32_t ALL_INTERRUPT_FLAGS{ DMA2D_IFCR_CTEIF | DMA2D_IFCR_CTCIF | DMA2D_IFCR_CTWIF |
                                                 DMA2D_IFCR_CAECIF | DMA2D_IFCR_CCTCIF | DMA2D_IFCR_CCEIF };
    bool accessible(std::uintptr_t address, std::size_t size, bool writable)
    {
        return std::ranges::any_of(RAM_REGIONS, [=](const Region& region) {
            return region.contains(address, size);
        }) || (!writable && QSPI.contains(address, size));
    }
    bool noncacheable(std::uintptr_t address, std::size_t size)
    {
        return UNCACHED_SDRAM.contains(address, size) || SHARED_RAM.contains(address, size);
    }
    void cache(std::span<const std::byte> bytes, bool invalidate)
    {
        const auto begin{ reinterpret_cast<std::uintptr_t>(bytes.data()) };
        // The framebuffer range is non-cacheable; QSPI assets are immutable.
        if (noncacheable(begin, bytes.size()) || QSPI.contains(begin, bytes.size())) {
            return;
        }
        const auto aligned{ begin & ~CACHE_LINE_MASK };
        const auto length{ (begin + bytes.size() + CACHE_LINE_MASK - aligned) & ~CACHE_LINE_MASK };
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
    profiling::interruptEnter();
    if (instance) {
        instance->interrupt();
    }
    profiling::interruptExit();
}
namespace hal
{
    Graphics::Graphics(DMA2D_TypeDef& registers)
      : m_registers{ registers }
    {
        stm32::InterruptGuard guard{};
        // CMSIS peripheral access uses the MCU fixed-address register map.
        // NOLINTNEXTLINE(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr,cppcoreguidelines-avoid-do-while)
        __HAL_RCC_DMA2D_CLK_ENABLE();
        instance = this;
        HAL_NVIC_SetPriority(DMA2D_IRQn, INTERRUPT_PRIORITY, 0);
        HAL_NVIC_EnableIRQ(DMA2D_IRQn);
    }
    Graphics::~Graphics()
    {
        abort();
        stm32::InterruptGuard guard{};
        HAL_NVIC_DisableIRQ(DMA2D_IRQn);
        HAL_NVIC_ClearPendingIRQ(DMA2D_IRQn);
        if (instance == this) {
            instance = nullptr;
        }
    }
    bool Graphics::busy() const noexcept
    {
        stm32::InterruptGuard guard{};
        return m_busy;
    }
    std::uint32_t Graphics::errors() const noexcept
    {
        stm32::InterruptGuard guard{};
        return m_errors;
    }
    auto Graphics::submit(const graphics::Transfer& t, graphics::Callback callback) -> util::Result<>
    {
        const auto destination{ reinterpret_cast<std::uintptr_t>(t.destination.data()) };
        if (!util::valid_graphics_transfer(t) || !accessible(destination, t.destination.size(), true) ||
            (!noncacheable(destination, t.destination.size()) &&
             (destination % CACHE_LINE_BYTES || t.destination.size() % CACHE_LINE_BYTES)) ||
            (t.operation != graphics::Operation::Fill && t.operation != graphics::Operation::BlendFill &&
             !accessible(reinterpret_cast<std::uintptr_t>(t.source.data()), t.source.size(), false))) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        stm32::InterruptGuard guard{};
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
        auto& r{ m_registers };
        r.CR = 0;
        r.IFCR = ALL_INTERRUPT_FLAGS;
        // Leave arbitration opportunities for LTDC's continuous SDRAM reads.
        // Program each submission too, since abort() resets this register.
        r.AMTCR = DMA2D_AMTCR_EN | (BUS_DEAD_TIME << DMA2D_AMTCR_DT_Pos);
        r.OPFCCR = DMA2D_OUTPUT_RGB565;
        r.OMAR = reinterpret_cast<std::uint32_t>(t.destination.data());
        r.OOR = (t.destination_stride / 2) - t.width;
        r.NLR = (t.width << DMA2D_NLR_PL_Pos) | t.height;
        std::uint32_t mode{};
        if (t.operation == graphics::Operation::Fill) {
            mode = DMA2D_R2M;
            r.OCOLR = util::rgb565(t.color);
        }
        else if (t.operation == graphics::Operation::BlendFill) {
            mode = DMA2D_M2M_BLEND_FG;
            const auto alpha{ (t.color >> ARGB_ALPHA_SHIFT) * t.alpha / ALPHA_MAX };
            r.FGCOLR = t.color & RGB_MASK;
            r.FGPFCCR = DMA2D_INPUT_ARGB8888 | (DMA2D_REPLACE_ALPHA << DMA2D_FGPFCCR_AM_Pos) |
                        (alpha << DMA2D_FGPFCCR_ALPHA_Pos);
            r.BGMAR = r.OMAR;
            r.BGOR = r.OOR;
            r.BGPFCCR = DMA2D_INPUT_RGB565 | (DMA2D_REPLACE_ALPHA << DMA2D_FGPFCCR_AM_Pos) |
                        (ALPHA_MAX << DMA2D_BGPFCCR_ALPHA_Pos);
        }
        else {
            const auto bytes{ t.source_format == graphics::Format::Rgb565 ? 2U : 4U };
            r.FGMAR = reinterpret_cast<std::uint32_t>(t.source.data());
            r.FGOR = (t.source_stride / bytes) - t.width;
            r.FGPFCCR =
              t.source_format == graphics::Format::Rgb565 ? DMA2D_INPUT_RGB565 : DMA2D_INPUT_ARGB8888;
            if (t.operation == graphics::Operation::Blend) {
                mode = DMA2D_M2M_BLEND;
                r.FGPFCCR |= (DMA2D_COMBINE_ALPHA << DMA2D_FGPFCCR_AM_Pos) |
                             (std::uint32_t(t.alpha) << DMA2D_FGPFCCR_ALPHA_Pos);
                r.BGMAR = r.OMAR;
                r.BGOR = r.OOR;
                r.BGPFCCR = DMA2D_INPUT_RGB565 | (DMA2D_REPLACE_ALPHA << DMA2D_FGPFCCR_AM_Pos) |
                            (ALPHA_MAX << DMA2D_BGPFCCR_ALPHA_Pos);
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
        const auto flags{ m_registers.ISR };
        m_registers.IFCR = flags;
        if (!m_busy || !(flags & (DMA2D_ISR_TCIF | DMA2D_ISR_TEIF | DMA2D_ISR_CEIF))) {
            return;
        }
        m_registers.CR = 0;
        const bool failed{ (flags & (DMA2D_ISR_TEIF | DMA2D_ISR_CEIF)) != 0U };
        if (failed) {
            ++m_errors;
        }
        cache(m_destination, true);
        m_busy = false;
        const auto callback{ m_callback };
        m_callback = {};
        callback(failed ? graphics::Completion::Error : graphics::Completion::Complete);
    }
    void Graphics::abort() noexcept
    {
        stm32::InterruptGuard guard{};
        if (m_busy) {
            // Reset synchronously terminates the bus master, even if an IRQ was lost.
            // CMSIS peripheral access uses the MCU fixed-address register map.
            // NOLINTNEXTLINE(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr,cppcoreguidelines-avoid-do-while)
            __HAL_RCC_DMA2D_FORCE_RESET();
            __DSB();
            // CMSIS peripheral access uses the MCU fixed-address register map.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr,cppcoreguidelines-avoid-do-while)
            __HAL_RCC_DMA2D_RELEASE_RESET();
            __DSB();
            cache(m_destination, true);
            m_busy = false;
            const auto callback{ m_callback };
            m_callback = {};
            callback(graphics::Completion::Aborted);
        }
        m_registers.IFCR = ALL_INTERRUPT_FLAGS;
        HAL_NVIC_ClearPendingIRQ(DMA2D_IRQn);
    }
}

// NOLINTEND(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr,cppcoreguidelines-avoid-do-while)
