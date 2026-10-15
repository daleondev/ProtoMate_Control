#pragma once
#include "hal/drivers/itf/IRgbOutput.hpp"
#include <mutex>
namespace hal
{
    class RgbOutput final : public IRgbOutput
    {
      public:
        ~RgbOutput() override;
        auto initialize() -> util::Result<> override;
        auto framebuffer(unsigned slot) -> std::span<std::uint16_t> override;
        auto present(unsigned slot) -> util::Result<> override;
        auto requestPresent(unsigned slot) -> util::Result<> override;
        auto state() const noexcept -> display::State override;
        void setCallback(display::Callback callback) noexcept override;
        void shutdown() noexcept override;
        void service() noexcept override;
        auto servicePeriod() const noexcept -> std::chrono::milliseconds override
        {
            return std::chrono::milliseconds{ 1 };
        }
        auto fill(unsigned slot, std::uint16_t rgb565) -> util::Result<> override;
        auto backlight(unsigned percent) -> util::Result<> override;
        auto underruns() const noexcept -> std::uint32_t override { return 0; }
        auto width() const noexcept -> unsigned override { return 1024; }
        auto height() const noexcept -> unsigned override { return 600; }

      private:
        bool m_initialized{};
        std::mutex m_callbackMutex;
        display::Callback m_callback;
        display::State m_observed;
    };
}
