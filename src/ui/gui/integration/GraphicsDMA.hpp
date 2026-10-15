#pragma once
#include "hal/drivers/itf/IGraphics.hpp"
#include "runtime/synchronization/Notification.hpp"
#include <atomic>
#include <memory>
#include <stop_token>
#include <touchgfx/hal/DMA.hpp>
namespace ui::gui
{
    class GraphicsDMA final : public touchgfx::DMA_Interface
    {
      public:
        GraphicsDMA(std::shared_ptr<hal::IGraphics> graphics, std::stop_token stop);
        ~GraphicsDMA() override;
        touchgfx::BlitOperations getBlitCaps() override;
        touchgfx::DMAType getDMAType() override { return touchgfx::DMA_TYPE_CHROMART; }
        void addToQueue(const touchgfx::BlitOp& operation) override;
        void flush() override {}
        void signalDMAInterrupt() override {} // IRQ callbacks only notify the worker
        bool verify(std::span<std::uint16_t> scratch);
        bool copyRegion(std::span<const std::uint16_t> source,
                        std::span<std::uint16_t> destination,
                        const touchgfx::Rect& region);
        bool failed() const noexcept { return m_failed; }
        std::uint32_t errors() const noexcept { return m_graphics ? m_graphics->errors() : 0; }
        std::uint64_t imageTransferUs() const noexcept { return m_imageTransferUs; }
        unsigned imageTransfers() const noexcept { return m_imageTransfers; }

      protected:
        void setupDataCopy(const touchgfx::BlitOp& operation) override { addToQueue(operation); }
        void setupDataFill(const touchgfx::BlitOp& operation) override { addToQueue(operation); }

      private:
        bool execute(const hal::graphics::Transfer& transfer);
        touchgfx::BlitOp m_storage{};
        touchgfx::LockFreeDMA_Queue m_queue;
        std::shared_ptr<hal::IGraphics> m_graphics;
        std::stop_token m_stop;
        runtime::Notification m_completion;
        std::atomic_bool m_done{};
        std::atomic<hal::graphics::Completion> m_result{};
        bool m_failed{};
        std::uint64_t m_imageTransferUs{};
        unsigned m_imageTransfers{};
    };
}
