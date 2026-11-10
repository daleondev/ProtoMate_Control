#pragma once
#include "hal/devices/itf/ITouch.hpp"
namespace hal::device
{
    // A portable input adapter; acquisition is injected by its platform factory.
    class PointerTouch final : public ITouch
    {
      public:
        explicit PointerTouch(TouchReport (*read)())
          : m_read{ read }
        {
        }
        auto initialize() -> util::Result<> override { return {}; }
        auto read() -> util::Result<TouchReport> override { return m_read(); }
        void setCallback(Callback /*callback*/, void* /*context*/) noexcept override {}
        auto idlePeriod() const noexcept -> std::chrono::milliseconds override
        {
            constexpr auto poll_interval{ std::chrono::milliseconds{ 8 } };
            return poll_interval;
        }

      private:
        TouchReport (*m_read)();
    };
}
