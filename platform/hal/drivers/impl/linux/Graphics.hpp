#pragma once
#include "hal/drivers/itf/IGraphics.hpp"
namespace hal
{
    class Graphics final : public IGraphics
    {
      public:
        auto submit(const graphics::Transfer& transfer, graphics::Callback callback)
          -> util::Result<> override;
        void abort() noexcept override {}
        bool busy() const noexcept override { return false; }
        std::uint32_t errors() const noexcept override { return 0; }
    };
}
