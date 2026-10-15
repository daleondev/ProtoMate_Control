#pragma once
#include "hal/devices/itf/ITouch.hpp"
namespace hal::device
{
    // A portable input adapter; acquisition is injected by its platform factory.
    class PointerTouch final : public ITouch
    {
      public:
        explicit PointerTouch(TouchReport (*read)())
          : m_read(read)
        {
        }
        auto initialize() -> util::Result<> override { return {}; }
        auto read() -> util::Result<TouchReport> override { return m_read(); }
        void setCallback(Callback, void*) noexcept override {}
        auto idlePeriod() const noexcept -> std::chrono::milliseconds override
        {
            return std::chrono::milliseconds{ 8 };
        }

      private:
        TouchReport (*m_read)();
    };
}
