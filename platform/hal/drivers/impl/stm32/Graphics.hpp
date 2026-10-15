#pragma once
#include "hal/drivers/itf/IGraphics.hpp"
#include "stm32h7xx_hal.h"
namespace hal
{
    class Graphics final : public IGraphics
    {
      public:
        explicit Graphics(DMA2D_TypeDef& registers);
        ~Graphics() override;
        auto submit(const graphics::Transfer& transfer, graphics::Callback callback)
          -> util::Result<> override;
        void abort() noexcept override;
        bool busy() const noexcept override;
        std::uint32_t errors() const noexcept override;
        void interrupt() noexcept;

      private:
        DMA2D_TypeDef& m_registers;
        graphics::Callback m_callback;
        std::span<std::byte> m_destination;
        std::uint32_t m_errors{};
        bool m_busy{};
    };
}
